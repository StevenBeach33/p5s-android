// Persona 5 Strikers's NVN shaders out of the player's game file, on the phone: the same output as games/p5r/import
// (what the translator reads: <out>/<group>/manifest.tsv, code_<sha1>.bin, ctrl_<sha1>.bin), found another way.
//
// Strikers is a Unity game. Unity keeps a shader's compiled programs in the Shader object, per platform, as
// LZ4-compressed sub-program blobs; on Switch each blob is NVN's own output, with the standard control section
// (magic 0x98761234) and code block (0x12345678, docs/nvn/SHADERS.md). Three layers, each cheap and checked:
//   1. the bytes of every RomFS file (and of the ExeFS main) are scanned for NVN blocks, which are paired with their
//      control section by the section's own size fields (the scan of tools/maxwell/exefs_shaders.py);
//   2. UnityFS bundles (LZ4 / LZ4HC; LZMA is reported, not read) are decompressed block by block first;
//   3. in all of that, Shader objects' offsets / compressedLengths / decompressedLengths / compressedBlob are found
//      by their shape (chained offsets, a blob as long as the lengths say, a first sub-blob that decodes) and each
//      sub-blob is LZ4-decompressed and scanned like layer 1.
// NOT YET RUN ON A REAL DUMP: the Unity layouts here come from the format as documented, not from a Strikers file.
// A run that finds nothing says what it saw (report.txt, and the import error), so the next step can be taken from it.
#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

#include "nxgame.h"

namespace p5s {

struct Fail : std::runtime_error {
    using std::runtime_error::runtime_error;
};

std::string sha1_16(const uint8_t* data, size_t n);                  // the translator's block names
void write_file(const std::string& path, const uint8_t* p, size_t n);  // throws Fail

// A file's bytes, read in pieces (a RomFS file of the game image, or a file on disk for the tests).
struct Source {
    virtual ~Source() = default;
    virtual uint64_t size() const = 0;
    virtual uint64_t read(uint64_t off, void* dst, uint64_t n) const = 0;  // the count read
};

struct Stats {
    uint64_t files = 0, bytes = 0, kids_files = 0;
    uint64_t unityfs = 0, unityfs_lzma = 0, unityfs_bad = 0;  // bundles seen; skipped for LZMA; unreadable
    uint64_t carve_candidates = 0, carved_blobs = 0, carved_with_nvn = 0;  // Shader blob arrays; blobs decoded; with NVN
    uint64_t raw_blocks = 0, carved_blocks = 0;  // new code blocks found in plain bytes / in decoded blobs
    std::map<std::string, long> stages;
};

class Window;

class Scanner {
public:
    explicit Scanner(std::string out_dir);
    // One file as one group: plain bytes and, for a UnityFS bundle, its decompressed blocks.
    void scan_file(const std::string& group, const Source& src);
    // One flat buffer (an ExeFS image, a test): plain bytes, then the Shader blob arrays in it.
    void scan_buffer(const std::string& group, const uint8_t* d, size_t n);
    // Writes the manifests. Distinct code blocks found so far.
    long finish();
    long blocks() const { return long(seen_.size()); }
    const Stats& stats() const { return stats_; }
    std::string report() const;

private:
    long scan_nvn(const std::string& group, const uint8_t* d, size_t n);
    bool unityfs(const Source& src, Window& w);

    std::string out_;
    std::set<std::string> seen_, dirs_;
    std::map<std::string, std::string> manifests_;
    Stats stats_;
};

// Distinct code blocks written under out_dir, or -1 with `error` set. progress(done, total) counts RomFS files.
long extract_shaders(const nxi::GameImage& game, const std::string& out_dir,
                     const std::function<void(int, int)>& progress, std::string& error);

// Media and data that hold no shaders and are most of a game's bytes (CRI audio and movies, fonts, images, json): a
// path ending in one of their extensions, with or without the hash Strikers appends to its CRI files
// (CriStreamingData/name.usm<32 hex>), or anywhere under a CriStreamingData folder.
bool is_media(std::string path);

// LZ4 block format, exactly `dst_size` bytes out. False on any malformed input.
bool lz4_block(const uint8_t* src, size_t src_size, uint8_t* dst, size_t dst_size);

}  // namespace p5s
