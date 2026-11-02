// The game image side of p5s_shaders.h: every RomFS file of the player's game, through the Scanner.
#include <algorithm>
#include <cctype>
#include <cstring>
#include <new>

#include "p5s_shaders.h"

namespace p5s {


namespace {

struct ImageSource : Source {
    ImageSource(const nxi::GameImage& g, const nxi::GameImage::Entry& e) : g_(g), e_(e) {}
    uint64_t size() const override { return e_.size; }
    uint64_t read(uint64_t off, void* dst, uint64_t n) const override { return g_.read(e_, off, dst, n); }
    const nxi::GameImage& g_;
    const nxi::GameImage::Entry& e_;
};

void walk(const nxi::GameImage& g, const std::string& dir,
          std::vector<std::pair<std::string, nxi::GameImage::Entry>>& files) {
    for (auto& [name, e] : g.list(dir)) {
        std::string path = dir.empty() ? name : dir + "/" + name;
        if (e.dir) walk(g, path, files);
        else files.push_back({path, e});
    }
}

std::string group_of(const std::string& path) {  // lower case, non-alphanumerics as '_', plus a short hash of the path
    std::string g;
    for (char c : path) {
        bool ok = std::isalnum(static_cast<unsigned char>(c)) || c == '_';
        if (ok) g += char(std::tolower(static_cast<unsigned char>(c)));
        else if (g.empty() || g.back() != '_') g += '_';
    }
    while (!g.empty() && g.back() == '_') g.pop_back();
    if (g.size() > 48) g = g.substr(g.size() - 48);
    return g + "_" + sha1_16(reinterpret_cast<const uint8_t*>(path.data()), path.size()).substr(0, 6);
}

}  // namespace

long extract_shaders(const nxi::GameImage& game, const std::string& out_dir,
                     const std::function<void(int, int)>& progress, std::string& error) {
    try {
        Scanner sc(out_dir);
        auto main_it = game.exefs().find("main");
        if (main_it != game.exefs().end()) {
            std::vector<uint8_t> flat;
            std::string e2;
            if (nxi::nso_to_flat_image(main_it->second, flat, e2)) sc.scan_buffer("exefs", flat.data(), flat.size());
        }
        std::vector<std::pair<std::string, nxi::GameImage::Entry>> files, todo;
        walk(game, "", files);
        for (auto& f : files)
            if (!is_media(f.first)) todo.push_back(f);
        const int total = int(todo.size());
        for (int i = 0; i < total; i++) {
            if (progress) progress(i, total);
            ImageSource src(game, todo[size_t(i)].second);
            sc.scan_file(group_of(todo[size_t(i)].first), src);
        }
        if (progress) progress(total, total);
        const long n = sc.finish();
        const std::string rep = sc.report();
        write_file(out_dir + "/report.txt", reinterpret_cast<const uint8_t*>(rep.data()), rep.size());
        if (n == 0) {
            error = "no NVN shaders found in the game file, so nothing can be translated. What was scanned:\n" + rep +
                    "(the full report is import-report.txt in the game's data folder; send it with the problem)";
            return -1;
        }
        return n;
    } catch (const Fail& e) {
        error = e.what();
    } catch (const std::bad_alloc&) {
        error = "out of memory";
    }
    return -1;
}

}  // namespace p5s
