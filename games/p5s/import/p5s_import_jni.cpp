// The import on the phone for Persona 5 Strikers (libp5simport.so, loaded by ImportActivity in its own process
// ":import"): the player's NSP or XCI and keys -> the game opened (the only title in the file, any version) and its
// shaders extracted (p5s_shaders.cpp: Unity's Shader objects, not Atlus' caches) and translated
// to SPIR-V (libspirvharness.so: the shader translator built with NativeAOT) into <data>/spirv. The game itself is
// read from the NSP at every start (runtime game_image.cpp): nothing else is copied.
//   Importer.run(nspFd, nspName, keysPath, titleKeysPath, dataDir, workDir, jobs) -> 0, or an error (Importer.error())
//   Importer.progress() -> {phase, done, total}: 1 opening, 2 extracting, 3 translating, 4 finishing, 5 done
#include <dirent.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <jni.h>
#include <sys/stat.h>
#include <unistd.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#include "nxgame.h"
#include "p5s_shaders.h"

namespace {

std::atomic<int> g_phase{0}, g_done{0}, g_total{0};
std::mutex g_error_lock;
std::string g_error, g_data_dir;

using HarnessMain = int (*)(int, const char**);
using HarnessProgress = void (*)(int*, int*);
HarnessProgress g_harness_progress = nullptr;

void set_error(const std::string& e) {
    std::lock_guard lk(g_error_lock);
    g_error = e;
}

std::string jstr(JNIEnv* env, jstring s) {
    if (!s) return {};
    const char* c = env->GetStringUTFChars(s, nullptr);
    std::string out = c ? c : "";
    if (c) env->ReleaseStringUTFChars(s, c);
    return out;
}

// The report of what the extraction saw, next to the game's data, where the player can find it and send it.
void copy_report(const std::string& from, const std::string& to) {
    FILE* in = std::fopen(from.c_str(), "rb");
    if (!in) return;
    if (FILE* out = std::fopen(to.c_str(), "wb")) {
        char buf[4096];
        size_t n;
        while ((n = std::fread(buf, 1, sizeof buf, in)) > 0) std::fwrite(buf, 1, n, out);
        std::fclose(out);
    }
    std::fclose(in);
}

void remove_tree(const std::string& path) {
    if (DIR* d = opendir(path.c_str())) {
        while (dirent* e = readdir(d)) {
            std::string n = e->d_name;
            if (n == "." || n == "..") continue;
            std::string p = path + "/" + n;
            struct stat st{};
            if (lstat(p.c_str(), &st) == 0 && S_ISDIR(st.st_mode)) remove_tree(p);
            else unlink(p.c_str());
        }
        closedir(d);
    }
    rmdir(path.c_str());
}

int fail(const std::string& e) {
    set_error(e);
    // The same text in a file the player can read and send (import-report.txt holds what the extraction saw before it).
    if (!g_data_dir.empty()) {
        if (FILE* f = std::fopen((g_data_dir + "/import-report.txt").c_str(), "a")) {
            std::fprintf(f, "\nIMPORT FAILED: %s\n", e.c_str());
            std::fclose(f);
        }
    }
    g_phase = 0;
    return 1;
}

int run(int nsp_fd, const std::string& nsp_name, const std::string& keys, const std::string& title_keys,
        const std::string& data_dir, const std::string& work_dir, int jobs) {
    g_phase = 1, g_done = 0, g_total = 0;
    set_error("");
    g_data_dir = data_dir;
    mkdir(data_dir.c_str(), 0700);
    unlink((data_dir + "/import-report.txt").c_str());
    // 1. the game file and the keys
    std::vector<nxi::InputFile> in;
    in.push_back({nsp_name, nsp_fd});
    int kfd = open(keys.c_str(), O_RDONLY | O_CLOEXEC);
    if (kfd < 0) return fail("cannot read prod.keys");
    in.push_back({"prod.keys", kfd});
    int tfd = title_keys.empty() ? -1 : open(title_keys.c_str(), O_RDONLY | O_CLOEXEC);
    if (tfd >= 0) in.push_back({"title.keys", tfd});
    std::string err;
    auto game = nxi::GameImage::open(in, 0, err);  // title 0: the only game in the file
    close(kfd);
    if (tfd >= 0) close(tfd);
    if (!game) return fail(err);
    auto main_it = game->exefs().find("main");
    if (main_it == game->exefs().end() || main_it->second.size() < 0x48) return fail("the game file has no main");
    char id[17];
    for (int i = 0; i < 8; i++) std::snprintf(id + 2 * i, 3, "%02X", main_it->second[0x40 + i]);
    // No build is required: Strikers's reference build is not known yet (games/p5s/game.cpp). The id goes in import.txt.

    // 2. the shaders out of the game
    g_phase = 2;
    remove_tree(work_dir);
    mkdir(work_dir.c_str(), 0700);
    const std::string extracted = work_dir + "/shaders";
    long blocks = p5s::extract_shaders(*game, extracted, [](int d, int t) { g_done = d, g_total = t; }, err);
    mkdir(data_dir.c_str(), 0700);
    copy_report(extracted + "/report.txt", data_dir + "/import-report.txt");
    if (blocks < 0) return fail(err);

    // 3. translated (the harness counts its own progress)
    g_phase = 3, g_done = 0, g_total = int(blocks);
    void* lib = dlopen("libspirvharness.so", RTLD_NOW);
    if (!lib) return fail(std::string("the shader translator is missing: ") + dlerror());
    auto harness = reinterpret_cast<HarnessMain>(dlsym(lib, "spirv_harness_main"));
    g_harness_progress = reinterpret_cast<HarnessProgress>(dlsym(lib, "spirv_harness_progress"));
    if (!harness || !g_harness_progress) return fail("the shader translator is incomplete");
    const std::string out_new = data_dir + "/spirv.new";
    remove_tree(out_new);
    std::string jobs_s = std::to_string(jobs > 0 ? jobs : 4);
    const char* args[] = {"--batch", extracted.c_str(), "--out", out_new.c_str(), "--jobs", jobs_s.c_str(), "--no-val"};
    int rc = harness(int(sizeof args / sizeof args[0]), args);
    if (rc != 0) return fail("the shader translation failed (code " + std::to_string(rc) + ")");

    // 4. in place of the old set; a note of what it was made from
    g_phase = 4;
    const std::string out = data_dir + "/spirv", old = data_dir + "/spirv.old";
    remove_tree(old);
    rename(out.c_str(), old.c_str());
    if (rename(out_new.c_str(), out.c_str()) != 0) return fail("cannot write " + out);
    remove_tree(old);
    remove_tree(work_dir);
    if (FILE* f = std::fopen((out + "/import.txt").c_str(), "w")) {
        std::fprintf(f, "%s\n%s\n%ld shaders\n", nsp_name.c_str(), id, blocks);
        std::fclose(f);
    }
    g_phase = 5;
    return 0;
}

}  // namespace

extern "C" {

JNIEXPORT jint JNICALL Java_io_github_p5sandroid_Importer_run(JNIEnv* env, jclass, jint nsp_fd, jstring nsp_name,
                                                             jstring keys, jstring title_keys, jstring data_dir,
                                                             jstring work_dir, jint jobs) {
    return run(nsp_fd, jstr(env, nsp_name), jstr(env, keys), jstr(env, title_keys), jstr(env, data_dir),
               jstr(env, work_dir), jobs);
}

JNIEXPORT jintArray JNICALL Java_io_github_p5sandroid_Importer_progress(JNIEnv* env, jclass) {
    int v[3] = {g_phase.load(), g_done.load(), g_total.load()};
    if (v[0] == 3 && g_harness_progress) g_harness_progress(&v[1], &v[2]);
    jintArray a = env->NewIntArray(3);
    env->SetIntArrayRegion(a, 0, 3, v);
    return a;
}

JNIEXPORT jstring JNICALL Java_io_github_p5sandroid_Importer_error(JNIEnv* env, jclass) {
    std::lock_guard lk(g_error_lock);
    return env->NewStringUTF(g_error.c_str());
}

}  // extern "C"
