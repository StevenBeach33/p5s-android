// p5s_shaders.h. The NVN block scan is tools/maxwell/exefs_shaders.py find() (as games/p5r/import/p5r_shaders.cpp
// has it), with the control sections looked up near each code block instead of in the whole buffer, because here the
// buffer can be a window of a very large file.
#include "p5s_shaders.h"

#include <sys/stat.h>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <zlib.h>

#if defined(_WIN32)
#include <direct.h>
#endif

namespace p5s {

namespace {

uint32_t le32(const uint8_t* p) { return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24; }
uint16_t be16(const uint8_t* p) { return uint16_t(p[0] << 8 | p[1]); }
uint32_t be32(const uint8_t* p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; }

// SHA-1: the translator names blocks by the first 16 hex digits of the code block's.
}  // namespace

std::string sha1_16(const uint8_t* data, size_t n) {
    uint32_t h[5] = {0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0};
    auto rol = [](uint32_t x, int s) { return (x << s) | (x >> (32 - s)); };
    auto block = [&](const uint8_t* p) {
        uint32_t w[80];
        for (int i = 0; i < 16; i++) w[i] = be32(p + 4 * i);
        for (int i = 16; i < 80; i++) w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
        uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
        for (int i = 0; i < 80; i++) {
            uint32_t f, k;
            if (i < 20) f = (b & c) | (~b & d), k = 0x5A827999;
            else if (i < 40) f = b ^ c ^ d, k = 0x6ED9EBA1;
            else if (i < 60) f = (b & c) | (b & d) | (c & d), k = 0x8F1BBCDC;
            else f = b ^ c ^ d, k = 0xCA62C1D6;
            uint32_t t = rol(a, 5) + f + e + k + w[i];
            e = d, d = c, c = rol(b, 30), b = a, a = t;
        }
        h[0] += a, h[1] += b, h[2] += c, h[3] += d, h[4] += e;
    };
    size_t i = 0;
    for (; i + 64 <= n; i += 64) block(data + i);
    uint8_t tail[128] = {};
    size_t rest = n - i;
    std::memcpy(tail, data + i, rest);
    tail[rest] = 0x80;
    size_t len = rest + 1 + 8 <= 64 ? 64 : 128;
    uint64_t bits = uint64_t(n) * 8;
    for (int j = 0; j < 8; j++) tail[len - 1 - j] = uint8_t(bits >> (8 * j));
    block(tail);
    if (len == 128) block(tail + 64);
    char out[41];
    for (int j = 0; j < 5; j++) std::snprintf(out + 8 * j, 9, "%08x", h[j]);
    return std::string(out, 16);
}

namespace {

void make_dir(const std::string& p) {
#if defined(_WIN32)
    _mkdir(p.c_str());
#else
    mkdir(p.c_str(), 0755);
#endif
}

}  // namespace

void write_file(const std::string& path, const uint8_t* p, size_t n) {
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f || std::fwrite(p, 1, n, f) != n) {
        if (f) std::fclose(f);
        throw Fail("cannot write " + path + " (" + std::strerror(errno) + ")");
    }
    if (std::fclose(f) != 0) throw Fail("cannot write " + path);
}

namespace {

// ---- NVN blocks ----

constexpr uint32_t kCtrlMagic = 0x98761234, kCode = 0x12345678, kCompute = 0x12345679;
constexpr uint64_t kExit = 0xe30000000007000full;
constexpr uint64_t kNear = 1u << 21;  // a code block's control section is within this many bytes of it

struct View {
    const uint8_t* d;
    uint64_t n;
    uint32_t u32(uint64_t o) const { return o + 4 <= n ? le32(d + o) : 0; }
    bool in(uint64_t o, uint64_t len) const { return o <= n && len <= n - o; }
    bool has_exit(uint64_t a, uint64_t b) const {  // the 8-byte EXIT anywhere in [a, b)
        uint8_t pat[8];
        for (int i = 0; i < 8; i++) pat[i] = uint8_t(kExit >> (8 * i));
        b = std::min<uint64_t>(b, n);
        return a < b && std::search(d + a, d + b, pat, pat + 8) != d + b;
    }
    bool zero(uint64_t a, uint64_t b) const {
        b = std::min<uint64_t>(b, n);
        for (uint64_t i = a; i < b; i++)
            if (d[i]) return false;
        return true;
    }
};

uint64_t up(uint64_t x) { return (x + 0xff) & ~uint64_t(0xff); }

const char* stage_of(const View& x, uint64_t p, uint64_t c) {
    if (x.u32(p) == kCompute || x.u32(c + 0x708) == 0) return "compute";
    uint32_t t = (x.u32(p + 0x30) >> 10) & 0xF;
    return t == 1 ? "vertex" : t == 5 ? "pixel" : "other";
}

// (code block, control section) pairs. Blocks whose control section is ambiguous are left out, as the P5R scan does.
std::vector<std::pair<uint64_t, uint64_t>> nvn_blocks(const View& x) {
    std::vector<uint64_t> ctrls, codes;
    for (uint64_t o = 0; o + 0x880 <= x.n; o += 4)
        if (x.u32(o) == kCtrlMagic && x.u32(o + 0x704) > 0 && x.u32(o + 0x704) < (1u << 24)) ctrls.push_back(o);
    if (ctrls.empty()) return {};
    for (uint64_t o = 0; o + 0x30 <= x.n; o++)
        if ((x.d[o] == 0x78 || x.d[o] == 0x79) && x.d[o + 1] == 0x56 && x.d[o + 2] == 0x34 && x.d[o + 3] == 0x12 &&
            x.zero(o + 4, o + 0x30)) {
            codes.push_back(o);
            o += 0x2f;
        }
    auto fits = [&](uint64_t p, uint64_t c) {
        uint64_t s = x.u32(c + 0x704), start = x.u32(c + 0x708), plen = x.u32(c + 0x6f8), csize = x.u32(c + 0x6fc),
                 coff = x.u32(c + 0x700);
        bool compute = x.u32(p) == kCompute;
        if (!x.in(p, s) || start != (compute ? 0x100u : 0x30u) || start + plen > s || plen < 8) return false;
        if (!x.has_exit(p + start, p + start + plen) || x.zero(p + start + plen - 8, p + start + plen)) return false;
        if (csize) {
            if (coff != up(start + plen) || s != up(coff + csize) || !x.zero(p + coff + csize, p + s)) return false;
        } else if (s != up(start + plen)) {
            return false;
        }
        if (!x.zero(p + start + plen, p + up(start + plen))) return false;
        uint32_t t = (x.u32(p + start) >> 10) & 0xF;
        return compute || t == 1 || t == 5;
    };
    auto key = [&](uint64_t c) {
        return std::string(reinterpret_cast<const char*>(x.d + c + 0x6f8), 0x14) +
               std::string(reinterpret_cast<const char*>(x.d + c + 0x748), 0x10);
    };
    std::vector<std::pair<uint64_t, uint64_t>> found;
    for (size_t i = 0; i < codes.size(); i++) {
        uint64_t p = codes[i], room = (i + 1 < codes.size() ? codes[i + 1] : x.n) - p;
        std::vector<uint64_t> cands;
        auto it = std::lower_bound(ctrls.begin(), ctrls.end(), p > kNear ? p - kNear : 0);
        for (; it != ctrls.end() && *it <= p + kNear; ++it)
            if (x.u32(*it + 0x704) <= room && fits(p, *it)) cands.push_back(*it);
        std::vector<uint64_t> adjacent;
        for (uint64_t c : cands)
            if (p >= c && p - c == 0x850) adjacent.push_back(c);
        if (adjacent.size() == 1) cands = adjacent;
        if (!cands.empty()) {  // a shorter size can also fit when the constant data happens to be zeros: the longest
            uint32_t size = 0;
            for (uint64_t c : cands) size = std::max(size, x.u32(c + 0x704));
            cands.erase(std::remove_if(cands.begin(), cands.end(), [&](uint64_t c) { return x.u32(c + 0x704) != size; }),
                        cands.end());
        }
        std::set<std::string> keys;
        for (uint64_t c : cands) keys.insert(key(c));
        if (keys.size() == 1) found.push_back({p, cands[0]});
    }
    return found;
}

// ---- Unity: the Shader object's sub-program blobs ----

// The arrays Unity serialises in a Shader (offsets, compressedLengths, decompressedLengths), each a vector of vectors
// (one per platform) or, in old versions, a single vector; then compressedBlob's size and bytes.
bool read_inner(const View& x, uint64_t& o, std::vector<uint32_t>& v) {
    uint32_t k = x.u32(o);
    if (k == 0 || k > 4096 || !x.in(o + 4, 4ull * k)) return false;
    v.resize(k);
    for (uint32_t i = 0; i < k; i++) v[i] = le32(x.d + o + 4 + 4ull * i);
    o += 4 + 4ull * k;
    return true;
}

bool read_arrays(const View& x, uint64_t& o, bool nested, std::vector<std::vector<uint32_t>>& out) {
    out.clear();
    uint32_t outer = 1;
    if (nested) {
        outer = x.u32(o);
        if (outer == 0 || outer > 64) return false;
        o += 4;
    }
    for (uint32_t i = 0; i < outer; i++) {
        std::vector<uint32_t> v;
        if (!read_inner(x, o, v)) return false;
        out.push_back(std::move(v));
    }
    return true;
}

using Sink = std::function<void(const uint8_t*, size_t)>;

// The end of the blob arrays starting at i, or 0 when what is there is not one (every check must hold; the first
// sub-blob must decode). Each decoded sub-blob goes to the sink.
uint64_t try_carve(const View& x, uint64_t i, bool nested, Stats& st, const Sink& sink) {
    uint64_t o = i;
    std::vector<std::vector<uint32_t>> offs, clen, dlen;
    if (!read_arrays(x, o, nested, offs) || !read_arrays(x, o, nested, clen) || !read_arrays(x, o, nested, dlen)) return 0;
    if (offs.size() != clen.size() || offs.size() != dlen.size() || offs[0][0] != 0) return 0;
    uint64_t total = 0;
    for (size_t p = 0; p < offs.size(); p++) {
        if (offs[p].size() != clen[p].size() || offs[p].size() != dlen[p].size()) return 0;
        for (size_t k = 0; k < offs[p].size(); k++) {
            uint64_t c = clen[p][k], d = dlen[p][k];
            if (c == 0 || d == 0 || d > (64u << 20)) return 0;
            if (c != d && c > d + d / 255 + 64) return 0;  // LZ4 cannot expand more than that
            if (k && offs[p][k] != uint64_t(offs[p][k - 1]) + clen[p][k - 1]) return 0;
            total = std::max<uint64_t>(total, uint64_t(offs[p][k]) + c);
        }
    }
    uint32_t len = x.u32(o);
    if (len < total || !x.in(o + 4, len)) return 0;
    const uint8_t* blob = x.d + o + 4;
    std::vector<uint8_t> buf;
    bool first = true;
    uint64_t decoded = 0;
    for (size_t p = 0; p < offs.size(); p++)
        for (size_t k = 0; k < offs[p].size(); k++) {
            buf.resize(dlen[p][k]);
            const uint8_t* src = blob + offs[p][k];
            bool ok = clen[p][k] == dlen[p][k] ? (std::memcpy(buf.data(), src, buf.size()), true)
                                               : lz4_block(src, clen[p][k], buf.data(), buf.size());
            if (first && !ok) return 0;
            first = false;
            if (!ok) continue;
            if (!decoded) st.carve_candidates++;
            decoded++;
            st.carved_blobs++;
            sink(buf.data(), buf.size());
        }
    return decoded ? o + 4 + len : 0;
}

void carve(const View& x, Stats& st, const Sink& sink) {
    for (uint64_t o = 0; o + 16 <= x.n; o += 4) {
        uint32_t a = x.u32(o), b = x.u32(o + 4), c = x.u32(o + 8);
        uint64_t end = 0;
        if (a >= 1 && a <= 64 && b >= 1 && b <= 4096 && c == 0) end = try_carve(x, o, true, st, sink);
        if (!end && a >= 1 && a <= 4096 && b == 0) end = try_carve(x, o, false, st, sink);
        if (end) o = ((end + 3) & ~uint64_t(3)) - 4;
    }
}

}  // namespace

bool lz4_block(const uint8_t* src, size_t sn, uint8_t* dst, size_t dn) {
    size_t s = 0, d = 0;
    while (s < sn) {
        uint8_t token = src[s++];
        size_t lit = token >> 4;
        if (lit == 15) {
            uint8_t b;
            do {
                if (s >= sn) return false;
                b = src[s++];
                lit += b;
            } while (b == 255);
        }
        if (lit > sn - s || lit > dn - d) return false;
        std::memcpy(dst + d, src + s, lit);
        s += lit, d += lit;
        if (s >= sn) break;  // the last sequence has literals only
        if (s + 2 > sn) return false;
        size_t off = size_t(src[s]) | size_t(src[s + 1]) << 8;
        s += 2;
        if (off == 0 || off > d) return false;
        size_t ml = token & 15;
        if (ml == 15) {
            uint8_t b;
            do {
                if (s >= sn) return false;
                b = src[s++];
                ml += b;
            } while (b == 255);
        }
        ml += 4;
        if (ml > dn - d) return false;
        for (size_t k = 0; k < ml; k++) dst[d + k] = dst[d + k - off];  // may overlap: byte by byte
        d += ml;
    }
    return d == dn;
}

// ---- Scanner ----

bool is_media(std::string path) {
    for (char& c : path) c = char(std::tolower(static_cast<unsigned char>(c)));
    if (path.find("cristreamingdata/") != std::string::npos) return true;
    static const char* kExt[] = {".usm", ".mp4", ".webm", ".ogg", ".wav", ".mp3", ".acb", ".awb", ".hca", ".adx", ".wem",
                                 ".bnk", ".fsb", ".png", ".jpg", ".dds", ".ktx", ".ttf", ".otf", ".txt", ".json", ".acf"};
    for (const char* e : kExt) {
        size_t p = path.rfind(e);
        if (p == std::string::npos) continue;
        const std::string rest = path.substr(p + std::strlen(e));  // nothing, or the hash
        if (rest.size() <= 40 && std::all_of(rest.begin(), rest.end(), [](char c) { return std::isxdigit(static_cast<unsigned char>(c)); }))
            return true;
    }
    return false;
}


// Bytes fed in pieces, scanned a window at a time (the tail of one window starts the next, so a block across the
// seam is still whole in one of them).
class Window {
public:
    Window(Scanner& s, std::string group) : s_(s), group_(std::move(group)) {}
    void feed(const uint8_t* p, size_t n) {
        buf_.insert(buf_.end(), p, p + n);
        if (buf_.size() >= kWindow + kOverlap) flush(false);
    }
    void finish() { flush(true); }

private:
    static constexpr size_t kWindow = 96u << 20, kOverlap = 8u << 20;
    void flush(bool last) {
        if (!buf_.empty()) s_.scan_buffer(group_, buf_.data(), buf_.size());
        if (last) {
            buf_.clear();
            buf_.shrink_to_fit();
        } else {
            // Drop a multiple of 16 bytes: Unity's objects are aligned in the file, and the scan steps by 4 from the start
            // of the buffer, so the buffer must keep starting at a multiple of 4 of the file.
            buf_.erase(buf_.begin(), buf_.begin() + ptrdiff_t((buf_.size() - kOverlap) & ~size_t(15)));
        }
    }
    Scanner& s_;
    std::string group_;
    std::vector<uint8_t> buf_;
};

Scanner::Scanner(std::string out_dir) : out_(std::move(out_dir)) { make_dir(out_); }

long Scanner::scan_nvn(const std::string& group, const uint8_t* d, size_t n) {
    View x{d, n};
    long fresh = 0;
    for (auto [p, c] : nvn_blocks(x)) {
        uint64_t s = x.u32(c + 0x704);
        if (!x.in(p, s) || !x.in(c, 0x880)) continue;
        std::string stage = stage_of(x, p, c);
        if (stage == "other") continue;
        std::string h = sha1_16(d + p, size_t(s));
        if (!seen_.insert(h).second) continue;
        std::string cf = "ctrl_" + sha1_16(d + c, 0x880) + ".bin";
        if (dirs_.insert(group).second) make_dir(out_ + "/" + group);
        write_file(out_ + "/" + group + "/code_" + h + ".bin", d + p, size_t(s));
        write_file(out_ + "/" + group + "/" + cf, d + c, 0x880);
        char line[512];
        std::snprintf(line, sizeof line,
                      "%s/0x%llx/%s\t%s\t0x%llx\t0x%llx\t0x%llx\t0x%x\t0x%x\t0x%x\t0x%x\t0x%x\t%s\tcode_%s.bin\t%s\n",
                      group.c_str(), (unsigned long long)p, stage.c_str(), stage.c_str(), (unsigned long long)p,
                      (unsigned long long)s, (unsigned long long)c, 0x880, x.u32(c + 0x708), x.u32(c + 0x6f8),
                      x.u32(c + 0x700), x.u32(c + 0x6fc), h.c_str(), h.c_str(), cf.c_str());
        manifests_[group] += line;
        stats_.stages[stage]++;
        fresh++;
    }
    return fresh;
}

void Scanner::scan_buffer(const std::string& group, const uint8_t* d, size_t n) {
    stats_.raw_blocks += uint64_t(scan_nvn(group, d, n));
    View x{d, n};
    carve(x, stats_, [&](const uint8_t* blob, size_t len) {
        long f = scan_nvn(group, blob, len);
        stats_.carved_blocks += uint64_t(f);
        if (f || (len >= 4 && std::search(blob, blob + len, reinterpret_cast<const uint8_t*>("\x34\x12\x76\x98"),
                                          reinterpret_cast<const uint8_t*>("\x34\x12\x76\x98") + 4) != blob + len))
            stats_.carved_with_nvn++;
    });
}

// UnityFS: the header, the block table (LZ4 / LZ4HC / stored), then each data block decompressed into the window.
bool Scanner::unityfs(const Source& src, Window& w) {
    stats_.unityfs++;
    const uint64_t fsz = src.size();
    std::vector<uint8_t> h(size_t(std::min<uint64_t>(fsz, 1024)));
    src.read(0, h.data(), h.size());
    size_t pos = 8;
    if (pos + 4 > h.size()) return stats_.unityfs_bad++, false;
    const uint32_t version = be32(&h[pos]);
    pos += 4;
    for (int i = 0; i < 2; i++) {  // the player version and the engine revision: C strings
        while (pos < h.size() && h[pos]) pos++;
        if (pos >= h.size()) return stats_.unityfs_bad++, false;
        pos++;
    }
    if (pos + 20 > h.size()) return stats_.unityfs_bad++, false;
    pos += 8;  // the bundle's size
    const uint32_t csize = be32(&h[pos]), usize = be32(&h[pos + 4]), flags = be32(&h[pos + 8]);
    pos += 12;
    if (version >= 7) pos = (pos + 15) & ~size_t(15);
    if (csize > (64u << 20) || usize > (256u << 20) || usize < 20) return stats_.unityfs_bad++, false;
    const bool info_at_end = flags & 0x80;
    const uint64_t info_off = info_at_end ? fsz - csize : pos;
    std::vector<uint8_t> raw(csize), info(usize);
    if (src.read(info_off, raw.data(), csize) != csize) return stats_.unityfs_bad++, false;
    const uint32_t ctype = flags & 0x3F;
    if (ctype == 0 && csize == usize) info = raw;
    else if ((ctype == 2 || ctype == 3) && lz4_block(raw.data(), raw.size(), info.data(), info.size())) {}
    else return (ctype == 1 ? stats_.unityfs_lzma : stats_.unityfs_bad)++, ctype == 1;
    if (info.size() < 20) return stats_.unityfs_bad++, false;
    const uint32_t count = be32(&info[16]);
    if (count == 0 || 20 + 10ull * count > info.size()) return stats_.unityfs_bad++, false;
    struct Block {
        uint32_t u, c, f;
    };
    std::vector<Block> blocks(count);
    for (uint32_t i = 0; i < count; i++) {
        const uint8_t* b = &info[20 + 10ull * i];
        blocks[i] = {be32(b), be32(b + 4), be16(b + 8)};
    }
    const uint64_t base = info_at_end ? pos : pos + csize, aligned = (base + 15) & ~uint64_t(15);
    uint64_t data = (flags & 0x200) ? aligned : base;
    std::vector<uint8_t> comp, out;
    auto decode = [&](uint64_t off, const Block& b) -> int {  // 1 ok, 0 unreadable, -1 LZMA
        if ((b.f & 0x3F) == 1) return -1;
        if (b.u > (64u << 20) || b.c > (64u << 20) || src.read(off, (comp.resize(b.c), comp.data()), b.c) != b.c) return 0;
        out.resize(b.u);
        if ((b.f & 0x3F) == 0) return b.c == b.u ? (std::memcpy(out.data(), comp.data(), b.u), 1) : 0;
        return lz4_block(comp.data(), comp.size(), out.data(), out.size()) ? 1 : 0;
    };
    // Some versions pad the start of the data, some do not and the flag does not say: the first block tells.
    if (decode(data, blocks[0]) == 0) {
        const uint64_t other = data == aligned ? base : aligned;
        if (other != data && decode(other, blocks[0]) == 1) data = other;
    }
    for (const Block& b : blocks) {
        int r = decode(data, b);
        if (r < 0) return stats_.unityfs_lzma++, true;
        if (r == 0) return stats_.unityfs_bad++, true;
        w.feed(out.data(), out.size());
        data += b.c;
    }
    return true;
}


// Koei Tecmo KIDS archives (IDRK / _DRK: *.rdb.bin, data/*.file): the shaders are in them as consecutive zlib streams of
// ~16 KB (one shader can span two of them), so the streams are inflated in file order and fed to the same window as plain
// bytes. A separator of zeros stands between runs that were not adjacent, so nothing is glued across a gap.
static bool kids_stream(const Source& src, Window& w) {
    const uint64_t N = src.size();
    constexpr size_t kLook = 8u << 20, kBuf = 24u << 20;
    std::vector<uint8_t> buf, out;
    uint64_t base = 0, i = 0, last_end = ~0ull;
    const uint8_t zeros[256] = {};
    auto load = [&](uint64_t at) {
        buf.resize(size_t(std::min<uint64_t>(kBuf, N - at)));
        base = at;
        buf.resize(size_t(src.read(at, buf.data(), buf.size())));
    };
    load(0);
    while (i + 2 < N) {
        if (i < base || i + kLook > base + buf.size()) {
            if (i < base || base + buf.size() < N) load(i);
            if (buf.empty()) break;
        }
        const uint8_t* p = buf.data() + (i - base);
        const size_t avail = buf.size() - size_t(i - base);
        if (avail < 2) break;
        const bool cand = p[0] == 0x78 && (p[1] == 0x9c || p[1] == 0xda || p[1] == 0x01 || p[1] == 0x5e);
        size_t used = 0;
        if (cand) {
            z_stream z = {};
            if (inflateInit(&z) == Z_OK) {
                z.next_in = const_cast<uint8_t*>(p);
                z.avail_in = uInt(std::min<size_t>(avail, kLook));
                out.clear();
                uint8_t tmp[1 << 16];
                int r = Z_OK;
                while (r == Z_OK && out.size() < (256u << 20)) {
                    z.next_out = tmp;
                    z.avail_out = sizeof tmp;
                    r = inflate(&z, Z_NO_FLUSH);
                    out.insert(out.end(), tmp, tmp + (sizeof tmp - z.avail_out));
                    if (z.avail_in == 0 && r == Z_OK) break;
                }
                if (r == Z_STREAM_END && out.size() >= 16) used = size_t(z.total_in);
                inflateEnd(&z);
            }
        }
        if (used) {
            if (last_end != ~0ull && i - last_end > 64) w.feed(zeros, sizeof zeros);
            w.feed(out.data(), out.size());
            i += used;
            last_end = i;
        } else {
            const uint8_t* q = static_cast<const uint8_t*>(std::memchr(p + 1, 0x78, avail - 1));
            i = q ? i + uint64_t(q - p) : i + avail;
        }
    }
    return true;
}

void Scanner::scan_file(const std::string& group, const Source& src) {
    stats_.files++;
    stats_.bytes += src.size();
    uint8_t head[8] = {};
    src.read(0, head, 8);
    Window w(*this, group);
    if ((std::memcmp(head, "IDRK", 4) == 0 || std::memcmp(head, "_DRK", 4) == 0) && src.size() >= 4096) {
        // the editors' resources (>1 GB, no game shaders seen in them) are not read
        if (src.size() <= (1100ull << 20) && group.find("editor") == std::string::npos && group.find("rrpreview") == std::string::npos) {
            stats_.kids_files++;
            kids_stream(src, w);
            w.finish();
        }
        return;
    }
    if (std::memcmp(head, "UnityFS", 7) == 0 && head[7] == 0 && unityfs(src, w)) {
        w.finish();
        return;
    }
    std::vector<uint8_t> chunk(size_t(std::min<uint64_t>(src.size(), 8u << 20)));  // thousands of small bundles
    for (uint64_t off = 0; off < src.size() && !chunk.empty();) {
        uint64_t got = src.read(off, chunk.data(), chunk.size());
        if (!got) break;
        w.feed(chunk.data(), size_t(got));
        off += got;
    }
    w.finish();
}

long Scanner::finish() {
    for (auto& [group, rows] : manifests_) {
        std::string text =
            "id\tstage\tcode_off\tcode_size\tctrl_off\tctrl_size\tprogram_start\tprogram_bytes\tconst_off\tconst_size\t"
            "code_sha1_16\tcode_file\tctrl_file\n" + rows;
        write_file(out_ + "/" + group + "/manifest.tsv", reinterpret_cast<const uint8_t*>(text.data()), text.size());
    }
    return blocks();
}

std::string Scanner::report() const {
    char b[1024];
    std::snprintf(b, sizeof b,
                  "files scanned: %llu (%llu MiB), KIDS archives read: %llu\nUnityFS bundles: %llu (LZMA, not read: %llu; unreadable: %llu)\n"
                  "Shader blob arrays found: %llu, sub-blobs decoded: %llu (with NVN magic: %llu)\n"
                  "code blocks: %ld (in plain bytes %llu, in decoded blobs %llu); vertex %ld, pixel %ld, compute %ld\n",
                  (unsigned long long)stats_.files, (unsigned long long)(stats_.bytes >> 20), (unsigned long long)stats_.kids_files,
                  (unsigned long long)stats_.unityfs, (unsigned long long)stats_.unityfs_lzma,
                  (unsigned long long)stats_.unityfs_bad, (unsigned long long)stats_.carve_candidates,
                  (unsigned long long)stats_.carved_blobs, (unsigned long long)stats_.carved_with_nvn, blocks(),
                  (unsigned long long)stats_.raw_blocks, (unsigned long long)stats_.carved_blocks,
                  stats_.stages.count("vertex") ? stats_.stages.at("vertex") : 0L,
                  stats_.stages.count("pixel") ? stats_.stages.at("pixel") : 0L,
                  stats_.stages.count("compute") ? stats_.stages.at("compute") : 0L);
    return b;
}

}  // namespace p5s
