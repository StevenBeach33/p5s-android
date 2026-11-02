// Host check of p5s_shaders.h on files you already have: p5s_scan <folder or file> <out dir>
// (a RomFS folder, or one Unity file). Prints what the importer prints on the phone; the blocks it finds are written
// in the layout the shader translator reads.
#include <fcntl.h>
#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <filesystem>

#include "p5s_shaders.h"

namespace fs = std::filesystem;

namespace {

struct FileSource : p5s::Source {
    explicit FileSource(const fs::path& p) : fd_(open(p.c_str(), O_RDONLY)), size_(fs::file_size(p)) {}
    ~FileSource() override {
        if (fd_ >= 0) close(fd_);
    }
    uint64_t size() const override { return size_; }
    uint64_t read(uint64_t off, void* dst, uint64_t n) const override {
        ssize_t r = pread(fd_, dst, size_t(n), off_t(off));
        return r < 0 ? 0 : uint64_t(r);
    }
    int fd_;
    uint64_t size_;
};

std::string group_name(const std::string& rel) {
    std::string g;
    for (char c : rel) g += (isalnum(static_cast<unsigned char>(c)) ? char(tolower(static_cast<unsigned char>(c))) : '_');
    return g + "_" + p5s::sha1_16(reinterpret_cast<const uint8_t*>(rel.data()), rel.size()).substr(0, 6);
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 3) return fprintf(stderr, "usage: p5s_scan <folder or file> <out dir>\n"), 2;
    const fs::path root = argv[1];
    p5s::Scanner sc(argv[2]);
    auto t0 = std::chrono::steady_clock::now();
    try {
        if (fs::is_directory(root)) {
            for (auto& e : fs::recursive_directory_iterator(root))
                if (e.is_regular_file() && !p5s::is_media(fs::relative(e.path(), root).string())) sc.scan_file(group_name(fs::relative(e.path(), root).string()), FileSource(e.path()));
        } else {
            sc.scan_file(group_name(root.filename().string()), FileSource(root));
        }
        sc.finish();
    } catch (const std::exception& e) {
        return fprintf(stderr, "error: %s\n", e.what()), 1;
    }
    double sec = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    printf("%s%.1f s\n", sc.report().c_str(), sec);
    return sc.blocks() > 0 ? 0 : 3;
}
