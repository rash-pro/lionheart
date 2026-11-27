#ifdef _WIN32
#undef fopen
#undef open
#undef mkdir
#undef unlink
#undef remove
#undef rename
#undef getenv
#endif

#include "ff8_mod_import.h"
#include "dds_decode.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <nlohmann/json.hpp>
#include <set>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <zlib.h>

#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#endif
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STBI_NO_HDR
#define STBI_NO_LINEAR
#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic pop
#endif

namespace HostHD {
namespace {

namespace fs = std::filesystem;
using json = nlohmann::json;

constexpr uint64_t kFnvOffset = UINT64_C(14695981039346656037);
constexpr uint64_t kFnvPrime = UINT64_C(1099511628211);

inline uint16_t le16(const uint8_t *p) {
    return uint16_t(p[0]) | uint16_t(uint16_t(p[1]) << 8);
}

inline int16_t le_s16(const uint8_t *p) {
    return int16_t(le16(p));
}

inline uint32_t le32(const uint8_t *p) {
    return uint32_t(p[0]) | (uint32_t(p[1]) << 8) |
           (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
}

inline uint64_t le64(const uint8_t *p) {
    return uint64_t(le32(p)) | (uint64_t(le32(p + 4)) << 32);
}

inline void write_be32(std::vector<uint8_t> &buf, uint32_t v) {
    buf.push_back(uint8_t((v >> 24) & 0xffu));
    buf.push_back(uint8_t((v >> 16) & 0xffu));
    buf.push_back(uint8_t((v >> 8) & 0xffu));
    buf.push_back(uint8_t(v & 0xffu));
}

inline void write_le32(std::vector<uint8_t> &buf, uint32_t v) {
    buf.push_back(uint8_t(v & 0xffu));
    buf.push_back(uint8_t((v >> 8) & 0xffu));
    buf.push_back(uint8_t((v >> 16) & 0xffu));
    buf.push_back(uint8_t((v >> 24) & 0xffu));
}

inline void write_le16(std::vector<uint8_t> &buf, uint16_t v) {
    buf.push_back(uint8_t(v & 0xffu));
    buf.push_back(uint8_t((v >> 8) & 0xffu));
}

inline void write_f32(std::vector<uint8_t> &buf, float f) {
    uint32_t bits = 0;
    std::memcpy(&bits, &f, 4);
    write_le32(buf, bits);
}

inline uint64_t fnv_u16(uint64_t h, uint16_t v) {
    h = (h ^ (v & 0xffu)) * kFnvPrime;
    h = (h ^ ((v >> 8) & 0xffu)) * kFnvPrime;
    return h;
}

std::string hex64(uint64_t v) {
    char buf[17];
    std::snprintf(buf, sizeof(buf), "%016llx", (unsigned long long)v);
    return std::string(buf, 16);
}

std::string to_lower(std::string s) {
    for (char &ch : s) {
        if (ch >= 'A' && ch <= 'Z') ch = char(ch + 32);
    }
    return s;
}

std::string trim(const std::string &s) {
    size_t a = 0;
    while (a < s.size() && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
    size_t b = s.size();
    while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) --b;
    return s.substr(a, b - a);
}

std::string normalize_slashes(std::string s) {
    std::replace(s.begin(), s.end(), '\\', '/');
    while (!s.empty() && s.front() == '/') s.erase(s.begin());
    while (!s.empty() && s.back() == '/') s.pop_back();
    return s;
}

/* UTF-16LE to UTF-8 without deprecated std::codecvt. */
std::string utf16le_to_utf8(const uint8_t *bytes, size_t byte_len) {
    std::string out;
    out.reserve(byte_len / 2);
    for (size_t i = 0; i + 1 < byte_len; i += 2) {
        uint32_t cp = le16(bytes + i);
        if (cp >= 0xD800 && cp <= 0xDBFF && i + 3 < byte_len) {
            uint32_t lo = le16(bytes + i + 2);
            if (lo >= 0xDC00 && lo <= 0xDFFF) {
                cp = 0x10000u + (((cp - 0xD800u) << 10) | (lo - 0xDC00u));
                i += 2;
            }
        }
        if (cp < 0x80) {
            out.push_back(char(cp));
        } else if (cp < 0x800) {
            out.push_back(char(0xC0u | (cp >> 6)));
            out.push_back(char(0x80u | (cp & 0x3Fu)));
        } else if (cp < 0x10000) {
            out.push_back(char(0xE0u | (cp >> 12)));
            out.push_back(char(0x80u | ((cp >> 6) & 0x3Fu)));
            out.push_back(char(0x80u | (cp & 0x3Fu)));
        } else {
            out.push_back(char(0xF0u | (cp >> 18)));
            out.push_back(char(0x80u | ((cp >> 12) & 0x3Fu)));
            out.push_back(char(0x80u | ((cp >> 6) & 0x3Fu)));
            out.push_back(char(0x80u | (cp & 0x3Fu)));
        }
    }
    return out;
}

/* ---------- Fast zlib PNG encoder ---------- */

void png_chunk(std::vector<uint8_t> &out, const char type[4], const uint8_t *data, size_t len) {
    write_be32(out, uint32_t(len));
    size_t start = out.size();
    out.insert(out.end(), type, type + 4);
    if (len) out.insert(out.end(), data, data + len);
    uLong crc = crc32(0L, Z_NULL, 0);
    crc = crc32(crc, out.data() + start, uInt(4 + len));
    write_be32(out, uint32_t(crc));
}

bool encode_rgba_png(uint32_t w, uint32_t h, const uint8_t *rgba, std::vector<uint8_t> &out) {
    if (!w || !h || !rgba) return false;
    size_t stride = size_t(w) * 4u;
    std::vector<uint8_t> raw(size_t(h) * (1u + stride));
    for (uint32_t y = 0; y < h; ++y) {
        uint8_t *dst = raw.data() + size_t(y) * (1u + stride);
        dst[0] = 0; /* filter type None (fast) */
        std::memcpy(dst + 1, rgba + size_t(y) * stride, stride);
    }

    uLongf comp_cap = compressBound(uLong(raw.size()));
    std::vector<uint8_t> comp(comp_cap);
    if (compress2(comp.data(), &comp_cap, raw.data(), uLong(raw.size()), Z_BEST_SPEED) != Z_OK) {
        return false;
    }
    comp.resize(size_t(comp_cap));

    out.clear();
    out.reserve(64 + comp.size());
    static const uint8_t kSig[8] = {137, 80, 78, 71, 13, 10, 26, 10};
    out.insert(out.end(), kSig, kSig + 8);

    uint8_t ihdr[13] = {};
    ihdr[0] = uint8_t((w >> 24) & 0xffu);
    ihdr[1] = uint8_t((w >> 16) & 0xffu);
    ihdr[2] = uint8_t((w >> 8) & 0xffu);
    ihdr[3] = uint8_t(w & 0xffu);
    ihdr[4] = uint8_t((h >> 24) & 0xffu);
    ihdr[5] = uint8_t((h >> 16) & 0xffu);
    ihdr[6] = uint8_t((h >> 8) & 0xffu);
    ihdr[7] = uint8_t(h & 0xffu);
    ihdr[8] = 8; /* bit depth */
    ihdr[9] = 6; /* RGBA */
    png_chunk(out, "IHDR", ihdr, sizeof(ihdr));
    png_chunk(out, "IDAT", comp.data(), comp.size());
    png_chunk(out, "IEND", nullptr, 0);
    return true;
}

bool write_file_bytes(const fs::path &path, const uint8_t *data, size_t size) {
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) return false;
    out.write(reinterpret_cast<const char *>(data), std::streamsize(size));
    return bool(out);
}

/* ---------- Image decode / crop / resize ---------- */

bool decode_image_bytes(const uint8_t *data, size_t size, DdsImage &out, std::string *error = nullptr) {
    if (!data || size < 16) {
        if (error) *error = "empty image";
        return false;
    }
    if (std::memcmp(data, "DDS ", 4) == 0) {
        return decode_dds(data, size, out, error);
    }
    int w = 0, h = 0, ch = 0;
    unsigned char *pix = stbi_load_from_memory(data, int(size), &w, &h, &ch, 4);
    if (!pix || w <= 0 || h <= 0) {
        if (pix) stbi_image_free(pix);
        if (error) *error = "failed to decode PNG/JPEG image";
        return false;
    }
    out.width = uint32_t(w);
    out.height = uint32_t(h);
    out.rgba.assign(pix, pix + size_t(w) * size_t(h) * 4u);
    stbi_image_free(pix);
    return true;
}

DdsImage crop_rgba(const DdsImage &src, uint32_t x0, uint32_t y0, uint32_t cw, uint32_t ch) {
    DdsImage dst;
    if (x0 >= src.width || y0 >= src.height || cw == 0 || ch == 0) return dst;
    cw = std::min(cw, src.width - x0);
    ch = std::min(ch, src.height - y0);
    dst.width = cw;
    dst.height = ch;
    dst.rgba.resize(size_t(cw) * size_t(ch) * 4u);
    for (uint32_t y = 0; y < ch; ++y) {
        std::memcpy(dst.rgba.data() + size_t(y) * cw * 4u,
                    src.rgba.data() + (size_t(y0 + y) * src.width + x0) * 4u,
                    size_t(cw) * 4u);
    }
    return dst;
}

DdsImage resize_rgba(const DdsImage &src, uint32_t dw, uint32_t dh) {
    if (src.width == dw && src.height == dh) return src;
    DdsImage dst;
    dst.width = dw;
    dst.height = dh;
    dst.rgba.resize(size_t(dw) * size_t(dh) * 4u);
    const float sx = float(src.width) / float(dw);
    const float sy = float(src.height) / float(dh);
    for (uint32_t y = 0; y < dh; ++y) {
        float fy = (float(y) + 0.5f) * sy - 0.5f;
        int y0 = int(std::floor(fy));
        float wy = fy - float(y0);
        int y1 = std::clamp(y0 + 1, 0, int(src.height) - 1);
        y0 = std::clamp(y0, 0, int(src.height) - 1);
        for (uint32_t x = 0; x < dw; ++x) {
            float fx = (float(x) + 0.5f) * sx - 0.5f;
            int x0 = int(std::floor(fx));
            float wx = fx - float(x0);
            int x1 = std::clamp(x0 + 1, 0, int(src.width) - 1);
            x0 = std::clamp(x0, 0, int(src.width) - 1);

            const uint8_t *p00 = src.rgba.data() + (size_t(y0) * src.width + size_t(x0)) * 4u;
            const uint8_t *p10 = src.rgba.data() + (size_t(y0) * src.width + size_t(x1)) * 4u;
            const uint8_t *p01 = src.rgba.data() + (size_t(y1) * src.width + size_t(x0)) * 4u;
            const uint8_t *p11 = src.rgba.data() + (size_t(y1) * src.width + size_t(x1)) * 4u;
            uint8_t *out = dst.rgba.data() + (size_t(y) * dw + x) * 4u;
            for (int c = 0; c < 4; ++c) {
                float top = p00[c] * (1.0f - wx) + p10[c] * wx;
                float bot = p01[c] * (1.0f - wx) + p11[c] * wx;
                int v = int(std::lround(top * (1.0f - wy) + bot * wy));
                out[c] = uint8_t(std::clamp(v, 0, 255));
            }
        }
    }
    return dst;
}

/* ---------- PS1 LZS Decompressor ---------- */

bool lzs_decompress(const uint8_t *src, size_t src_len, std::vector<uint8_t> &out) {
    if (!src || src_len < 4) return false;
    uint32_t comp_len = le32(src);
    if (comp_len == 0 || comp_len > src_len - 4) comp_len = uint32_t(src_len - 4);
    const uint8_t *p = src + 4;
    const uint8_t *end = p + comp_len;

    out.clear();
    out.reserve(comp_len * 3);
    uint8_t ring[4096] = {};
    unsigned rpos = 0xFEEu;

    while (p < end) {
        uint8_t flags = *p++;
        for (int bit = 0; bit < 8 && p < end; ++bit) {
            if (flags & (1u << bit)) {
                uint8_t b = *p++;
                out.push_back(b);
                ring[rpos] = b;
                rpos = (rpos + 1u) & 0xFFFu;
            } else {
                if (p + 2 > end) break;
                uint8_t b1 = *p++;
                uint8_t b2 = *p++;
                unsigned off = unsigned(b1) | (unsigned(b2 & 0xF0u) << 4);
                unsigned len = unsigned(b2 & 0x0Fu) + 3u;
                for (unsigned k = 0; k < len; ++k) {
                    uint8_t b = ring[(off + k) & 0xFFFu];
                    out.push_back(b);
                    ring[rpos] = b;
                    rpos = (rpos + 1u) & 0xFFFu;
                }
            }
        }
    }
    return !out.empty();
}

/* ---------- PS1 Disc (.cue / .bin / .iso) Reader ---------- */

class DiscReader {
public:
    bool open(const fs::path &cue_or_bin, std::string *error = nullptr) {
        fs::path bin_path = cue_or_bin;
        std::string ext = to_lower(cue_or_bin.extension().string());
        if (ext == ".cue") {
            std::ifstream cue(cue_or_bin);
            if (!cue) {
                if (error) *error = "cannot open CUE: " + cue_or_bin.string();
                return false;
            }
            std::string line;
            bool found = false;
            while (std::getline(cue, line)) {
                std::string t = trim(line);
                if (t.size() >= 5 && to_lower(t.substr(0, 4)) == "file" &&
                    std::isspace(static_cast<unsigned char>(t[4]))) {
                    size_t q1 = t.find('"');
                    std::string fname;
                    if (q1 != std::string::npos) {
                        size_t q2 = t.find('"', q1 + 1);
                        if (q2 != std::string::npos) fname = t.substr(q1 + 1, q2 - q1 - 1);
                    } else {
                        size_t sp = t.find_first_of(" \t", 5);
                        fname = trim(t.substr(4, sp == std::string::npos ? std::string::npos : sp - 4));
                    }
                    if (!fname.empty()) {
                        fs::path candidate = cue_or_bin.parent_path() / fname;
                        if (fs::exists(candidate)) {
                            bin_path = candidate;
                            found = true;
                            break;
                        }
                        /* Case-insensitive fallback in same directory */
                        std::string want = to_lower(fname);
                        std::error_code ec;
                        for (const auto &ent : fs::directory_iterator(cue_or_bin.parent_path(), ec)) {
                            if (to_lower(ent.path().filename().string()) == want) {
                                bin_path = ent.path();
                                found = true;
                                break;
                            }
                        }
                        if (found) break;
                    }
                }
            }
            if (!found) {
                if (error) *error = "cannot locate BIN referenced by CUE: " + cue_or_bin.string();
                return false;
            }
        }

        file_.open(bin_path, std::ios::binary);
        if (!file_) {
            if (error) *error = "cannot open disc binary: " + bin_path.string();
            return false;
        }
        file_.seekg(0, std::ios::end);
        file_size_ = uint64_t(file_.tellg());

        /* Probe sector layout at LBA 16 ("CD001"). */
        uint8_t probe[6] = {};
        if (file_size_ > 16u * 2352u + 30u) {
            file_.seekg(std::streamoff(16u * 2352u + 24u + 1u));
            file_.read(reinterpret_cast<char *>(probe), 5);
            if (std::memcmp(probe, "CD001", 5) == 0) {
                sector_size_ = 2352;
                data_offset_ = 24;
                return true;
            }
        }
        if (file_size_ > 16u * 2048u + 6u) {
            file_.seekg(std::streamoff(16u * 2048u + 1u));
            file_.read(reinterpret_cast<char *>(probe), 5);
            if (std::memcmp(probe, "CD001", 5) == 0) {
                sector_size_ = 2048;
                data_offset_ = 0;
                return true;
            }
        }
        if (error) *error = "not an ISO9660 PlayStation disc image: " + bin_path.string();
        return false;
    }

    bool read_sector(uint32_t lba, uint8_t out[2048]) {
        uint64_t off = uint64_t(lba) * sector_size_ + data_offset_;
        if (off + 2048u > file_size_) return false;
        file_.seekg(std::streamoff(off));
        file_.read(reinterpret_cast<char *>(out), 2048);
        return file_.gcount() == 2048;
    }

    bool read_bytes(uint32_t lba, uint32_t size, std::vector<uint8_t> &out) {
        out.clear();
        if (size == 0) return true;
        out.resize(size);
        uint32_t remaining = size;
        uint32_t cur_lba = lba;
        size_t dst_off = 0;
        uint8_t sec[2048];
        while (remaining > 0) {
            if (!read_sector(cur_lba, sec)) return false;
            uint32_t take = std::min<uint32_t>(remaining, 2048u);
            std::memcpy(out.data() + dst_off, sec, take);
            dst_off += take;
            remaining -= take;
            ++cur_lba;
        }
        return true;
    }

    bool find_ff8_img(uint32_t &out_lba, uint32_t &out_size, int &out_disc_num) {
        uint8_t pvd[2048];
        if (!read_sector(16, pvd)) return false;
        uint32_t root_lba = le32(pvd + 156 + 2);
        uint32_t root_size = le32(pvd + 156 + 10);
        std::vector<uint8_t> dir;
        if (!read_bytes(root_lba, root_size, dir)) return false;

        size_t pos = 0;
        while (pos < dir.size()) {
            uint8_t rec_len = dir[pos];
            if (rec_len == 0) {
                pos = ((pos / 2048u) + 1u) * 2048u;
                continue;
            }
            if (pos + rec_len > dir.size() || rec_len < 34) break;
            uint8_t name_len = dir[pos + 32];
            if (33u + name_len <= rec_len) {
                std::string name(reinterpret_cast<const char *>(dir.data() + pos + 33), name_len);
                size_t semi = name.find(';');
                if (semi != std::string::npos) name.resize(semi);
                std::string up = name;
                for (char &c : up) if (c >= 'a' && c <= 'z') c = char(c - 32);
                if (up.size() == 12 && up.substr(0, 7) == "FF8DISC" && up.substr(8) == ".IMG" &&
                    up[7] >= '1' && up[7] <= '4') {
                    out_lba = le32(dir.data() + pos + 2);
                    out_size = le32(dir.data() + pos + 10);
                    out_disc_num = up[7] - '0';
                    return true;
                }
            }
            pos += rec_len;
        }
        return false;
    }

private:
    std::ifstream file_;
    uint64_t file_size_ = 0;
    uint32_t sector_size_ = 2352;
    uint32_t data_offset_ = 24;
};

/* ---------- TIM Parser & Hash Computation ---------- */

struct TimParsed {
    uint32_t bpp = 0;       /* 0=4bpp, 1=8bpp, 2=16bpp */
    uint16_t vram_w = 0;    /* 16-bit VRAM words */
    uint16_t vram_h = 0;
    uint32_t px_w = 0;      /* pixel width */
    uint32_t px_h = 0;
    uint64_t tex_hash = 0;
    std::vector<uint64_t> clut_hashes;
    const uint8_t *img_bytes = nullptr;
    size_t next_off = 0;
};

uint64_t hash_words_slice(const uint8_t *img_bytes, uint16_t vram_w, uint16_t y0, uint16_t y1) {
    uint64_t h = kFnvOffset;
    h = fnv_u16(h, vram_w);
    h = fnv_u16(h, uint16_t(y1 - y0));
    for (uint16_t y = y0; y < y1; ++y) {
        const uint8_t *row = img_bytes + size_t(y) * size_t(vram_w) * 2u;
        for (uint16_t x = 0; x < vram_w; ++x) {
            h = fnv_u16(h, le16(row + size_t(x) * 2u));
        }
    }
    return h;
}

bool parse_tim_at(const uint8_t *buf, size_t len, size_t off, TimParsed &out) {
    if (off + 20 > len) return false;
    const uint8_t *p = buf + off;
    if (le32(p) != 0x10u) return false;
    uint32_t flags = le32(p + 4);
    uint32_t bpp = flags & 7u;
    if (bpp > 2u || (flags & ~0xFu) != 0u) return false;
    bool has_clut = (flags & 8u) != 0u;

    size_t pos = 8;
    out.clut_hashes.clear();
    if (has_clut) {
        if (off + pos + 12 > len) return false;
        uint32_t clut_len = le32(p + pos);
        if (clut_len < 12 || off + pos + clut_len > len) return false;
        uint16_t cw = le16(p + pos + 8);
        uint16_t ch = le16(p + pos + 10);
        if (cw == 0 || ch == 0 || cw > 1024 || ch > 256) return false;
        if (12u + uint64_t(cw) * ch * 2u > clut_len) return false;
        const uint8_t *cdata = p + pos + 12;
        uint32_t pal_size = (bpp == 0) ? 16u : 256u;
        if (cw >= pal_size) {
            for (uint16_t row = 0; row < ch; ++row) {
                uint64_t chash = kFnvOffset;
                const uint8_t *rptr = cdata + size_t(row) * cw * 2u;
                for (uint32_t i = 0; i < pal_size; ++i) {
                    chash = fnv_u16(chash, le16(rptr + size_t(i) * 2u));
                }
                out.clut_hashes.push_back(chash);
            }
        }
        pos += clut_len;
    }

    if (off + pos + 12 > len) return false;
    uint32_t img_len = le32(p + pos);
    if (img_len < 12 || off + pos + img_len > len) return false;
    uint16_t w = le16(p + pos + 8);
    uint16_t h = le16(p + pos + 10);
    if (w == 0 || h == 0 || w > 1024 || h > 512) return false;
    if (12u + uint64_t(w) * h * 2u != img_len) return false;

    const uint8_t *idata = p + pos + 12;
    out.bpp = bpp;
    out.vram_w = w;
    out.vram_h = h;
    out.px_w = (bpp == 0) ? uint32_t(w) * 4u : (bpp == 1) ? uint32_t(w) * 2u : uint32_t(w);
    out.px_h = h;
    out.img_bytes = idata;
    out.tex_hash = hash_words_slice(idata, w, 0, h);
    out.next_off = off + pos + img_len;
    return true;
}

std::vector<TimParsed> scan_embedded_tims(const uint8_t *buf, size_t len) {
    std::vector<TimParsed> res;
    size_t off = 0;
    while (off + 20 <= len) {
        if (buf[off] == 0x10 && buf[off + 1] == 0 && buf[off + 2] == 0 && buf[off + 3] == 0) {
            TimParsed t;
            if (parse_tim_at(buf, len, off, t)) {
                res.push_back(t);
                off = (t.next_off + 3u) & ~size_t(3u);
                continue;
            }
        }
        off += 4;
    }
    return res;
}

/* ---------- PS1 FF8 Disc Index ---------- */

struct TexTarget {
    uint64_t tex_hash = 0;
    std::vector<uint64_t> clut_hashes;
    uint32_t target_w = 0; /* expected full pixel width */
    uint32_t target_h = 0; /* expected full pixel height */
    bool is_subregion = false;
    uint32_t sub_y0 = 0;   /* PS1-space row start (out of full_h) */
    uint32_t sub_y1 = 0;   /* PS1-space row end (exclusive) */
    uint32_t full_h = 0;   /* PS1-space full height */
    bool is_battle_stage = false;
};

struct Ps1ModelInfo {
    uint32_t tim_count = 0;
    uint32_t vert_count = 0;
    std::vector<uint16_t> skin_bones;
    std::vector<uint64_t> vram_texture_hashes;
};

struct Ps1Index {
    /* Key: canonical lowercase FFNx texture key, e.g. "d000-0", "a0stg101-0", "c0m028-0",
     * "d0c000-0", "wmset_section38-0". Value: one or more VRAM targets (full + splits). */
    std::unordered_map<std::string, std::vector<TexTarget>> textures;
    /* Key: "d000".."d075" */
    std::unordered_map<std::string, Ps1ModelInfo> field_models;
    /* Key: "c0m000".."c0m143", "d0c000".."dac022", "d0w000".."d9w037" */
    std::unordered_map<std::string, Ps1ModelInfo> battle_models;
    /* Key: "c0w000", "squall", "car", "garden", "ragnarok", "chocobo", "wmset_section38" */
    std::unordered_map<std::string, Ps1ModelInfo> world_models;
};

/* Parse bone count, vertex count, and TIM count from an FF8 battle .dat buffer. */
void inspect_battle_dat_ps1(const std::vector<uint8_t> &dat, Ps1ModelInfo &out) {
    if (dat.size() < 16) return;
    uint32_t sec_count = le32(dat.data());
    if (sec_count < 3 || sec_count > 32 || 4u + (sec_count + 1u) * 4u > dat.size()) return;
    uint32_t off_skel = le32(dat.data() + 4u + 0u * 4u);
    uint32_t off_geom = le32(dat.data() + 4u + 1u * 4u);
    uint32_t off_tex = le32(dat.data() + 4u + (sec_count - 1u) * 4u);
    if (off_skel + 2u <= dat.size()) {
        uint16_t nbones = le16(dat.data() + off_skel);
        if (nbones <= 256) {
            out.skin_bones.clear();
            for (uint16_t b = 0; b < nbones; ++b) out.skin_bones.push_back(b);
        }
    }
    if (off_geom + 4u <= dat.size()) {
        uint32_t nobjs = le32(dat.data() + off_geom);
        if (nobjs > 0 && nobjs <= 64 && off_geom + 4u + nobjs * 4u <= dat.size()) {
            uint32_t total_v = 0;
            for (uint32_t oi = 0; oi < nobjs; ++oi) {
                uint32_t oo = off_geom + le32(dat.data() + off_geom + 4u + oi * 4u);
                if (oo + 2u > dat.size()) continue;
                uint16_t ng = le16(dat.data() + oo);
                size_t p = oo + 2u;
                for (uint16_t g = 0; g < ng && p + 4u <= dat.size(); ++g) {
                    uint16_t vc = le16(dat.data() + p + 2u);
                    p += 4u + size_t(vc) * 6u;
                    total_v += vc;
                }
            }
            out.vert_count = total_v;
        }
    }
    if (off_tex + 4u <= dat.size()) {
        uint32_t ntims = le32(dat.data() + off_tex);
        if (ntims <= 64) out.tim_count = ntims;
    }
}

struct BattleCharEntry {
    int slot;
    const char *name;
};

constexpr BattleCharEntry kBattleChars[] = {
    {311, "d0c000"}, {312, "d0c001"},
    {313, "d0w000"}, {314, "d0w001"}, {315, "d0w002"}, {316, "d0w003"},
    {317, "d0w004"}, {318, "d0w005"}, {319, "d0w006"},
    {321, "d1c003"}, {322, "d1c004"},
    {323, "d1w008"}, {324, "d1w009"}, {325, "d1w010"}, {326, "d1w011"},
    {327, "d2c006"},
    {328, "d2w013"}, {329, "d2w014"}, {330, "d2w015"}, {331, "d2w016"},
    {332, "d3c007"},
    {333, "d3w018"}, {334, "d3w019"}, {335, "d3w020"}, {336, "d3w021"},
    {337, "d4c009"},
    {338, "d4w023"}, {339, "d4w024"}, {340, "d4w025"}, {341, "d4w026"}, {342, "d4w027"},
    {343, "d5c011"}, {344, "d5c012"},
    {345, "d5w028"}, {346, "d5w029"}, {347, "d5w030"}, {348, "d5w031"},
    {349, "d6c014"}, {350, "d6w033"},
    {351, "d7c016"},
    {352, "d8c017"}, {353, "d8c018"}, {354, "d8w035"},
    {355, "d9c019"}, {356, "d9c020"}, {357, "d9w037"},
    {358, "dac021"}, {359, "dac022"},
};

void add_tim_target(Ps1Index &idx, const std::string &key, const TimParsed &t,
                    bool battle_stage, bool add_field_splits,
                    std::vector<uint64_t> *model_hashes = nullptr) {
    auto &vec = idx.textures[key];
    auto push_unique = [&](const TexTarget &tt) {
        for (const auto &e : vec) {
            if (e.tex_hash == tt.tex_hash && e.is_subregion == tt.is_subregion &&
                e.sub_y0 == tt.sub_y0 && e.sub_y1 == tt.sub_y1) {
                return;
            }
        }
        vec.push_back(tt);
    };

    TexTarget full;
    full.tex_hash = t.tex_hash;
    full.clut_hashes = t.clut_hashes;
    full.target_w = t.px_w;
    full.target_h = t.px_h;
    full.full_h = t.px_h;
    full.is_battle_stage = battle_stage;
    push_unique(full);
    if (model_hashes) model_hashes->push_back(t.tex_hash);

    /* FF8 field character 64x128 VRAM sheets are split by a 1-row CLUT strip at
     * VRAM y=240..247 (character slots 0..7 -> split rows 112..119). Pre-register
     * all slots in model_hashes so every party member / NPC slot matches its .glb! */
    if (add_field_splits && t.vram_w == 64 && t.vram_h == 128 && t.img_bytes) {
        for (uint16_t split = 112; split <= 119; ++split) {
            uint64_t h_top = hash_words_slice(t.img_bytes, t.vram_w, 0, split);
            uint64_t h_bot = hash_words_slice(t.img_bytes, t.vram_w, uint16_t(split + 1), 128);
            if (model_hashes) {
                model_hashes->push_back(h_top);
                model_hashes->push_back(h_bot);
            }
            TexTarget top;
            top.tex_hash = h_top;
            top.clut_hashes = t.clut_hashes;
            top.target_w = t.px_w;
            top.target_h = split;
            top.is_subregion = true;
            top.sub_y0 = 0;
            top.sub_y1 = split;
            top.full_h = 128;
            push_unique(top);

            TexTarget bot;
            bot.tex_hash = h_bot;
            bot.clut_hashes = t.clut_hashes;
            bot.target_w = t.px_w;
            bot.target_h = uint32_t(128 - (split + 1));
            bot.is_subregion = true;
            bot.sub_y0 = uint32_t(split + 1);
            bot.sub_y1 = 128;
            bot.full_h = 128;
            push_unique(bot);
        }
    }
}

bool index_ps1_disc(const fs::path &disc_path, Ps1Index &idx, std::string *error) {
    DiscReader disc;
    if (!disc.open(disc_path, error)) return false;

    uint32_t img_lba = 0, img_size = 0;
    int disc_num = 0;
    if (!disc.find_ff8_img(img_lba, img_size, disc_num)) {
        if (error) *error = "FF8DISC*.IMG not found on disc: " + disc_path.string();
        return false;
    }

    uint8_t top_raw[2048];
    if (!disc.read_sector(img_lba, top_raw)) {
        if (error) *error = "cannot read FF8DISC top table";
        return false;
    }
    auto top_entry = [&](int i) -> std::pair<uint32_t, uint32_t> {
        return {le32(top_raw + i * 8), le32(top_raw + i * 8 + 4)};
    };

    /* 1. Field main characters (module 2, LZS, table @ 0x286a0, 76 entries). */
    {
        auto [m2_lba, m2_sz] = top_entry(2);
        std::vector<uint8_t> m2_comp, m2;
        if (m2_sz > 0 && disc.read_bytes(m2_lba, m2_sz, m2_comp) &&
            lzs_decompress(m2_comp.data(), m2_comp.size(), m2) &&
            m2.size() >= 0x286a0u + 76u * 8u) {
            for (int i = 0; i < 76; ++i) {
                uint32_t lba = le32(m2.data() + 0x286a0u + i * 8);
                uint32_t sz = le32(m2.data() + 0x286a0u + i * 8 + 4);
                if (!lba || !sz || sz > 4u * 1024u * 1024u) continue;
                std::vector<uint8_t> mch;
                if (!disc.read_bytes(lba, sz, mch) || mch.size() < 16) continue;

                char id_buf[16];
                std::snprintf(id_buf, sizeof(id_buf), "d%03d", i);
                std::string mid(id_buf);

                std::vector<uint32_t> tim_offs;
                size_t p = 0;
                while (p + 4 <= mch.size()) {
                    uint32_t v = le32(mch.data() + p);
                    p += 4;
                    if (v == 0xFFFFFFFFu) break;
                    tim_offs.push_back(v & 0xFFFFFFu);
                }
                if (p + 4 > mch.size()) continue;
                uint32_t moff = le32(mch.data() + p);

                Ps1ModelInfo &minfo = idx.field_models[mid];
                minfo.tim_count = uint32_t(tim_offs.size());
                if (moff + 64 <= mch.size()) {
                    minfo.vert_count = le32(mch.data() + moff + 1 * 4);
                    uint32_t c_skin = le32(mch.data() + moff + 5 * 4);
                    uint32_t off_skin = le32(mch.data() + moff + 13 * 4);
                    if (c_skin <= 256 && moff + off_skin + c_skin * 8u <= mch.size()) {
                        minfo.skin_bones.clear();
                        for (uint32_t s = 0; s < c_skin; ++s) {
                            const uint8_t *sp = mch.data() + moff + off_skin + s * 8u;
                            uint16_t bid = le16(sp + 4u);
                            minfo.skin_bones.push_back(bid);
                        }
                    }
                }

                for (size_t ti = 0; ti < tim_offs.size(); ++ti) {
                    TimParsed t;
                    if (parse_tim_at(mch.data(), mch.size(), tim_offs[ti], t)) {
                        add_tim_target(idx, mid + "-" + std::to_string(ti), t,
                                       false, true, &minfo.vram_texture_hashes);
                    }
                }
                /* Deduplicate model_hashes */
                std::sort(minfo.vram_texture_hashes.begin(), minfo.vram_texture_hashes.end());
                minfo.vram_texture_hashes.erase(
                    std::unique(minfo.vram_texture_hashes.begin(), minfo.vram_texture_hashes.end()),
                    minfo.vram_texture_hashes.end());
            }
        }
    }

    /* 2. Battle table (module 25, UNCOMPRESSED, @ 0x499b4, 1118 entries). */
    {
        auto [m25_lba, m25_sz] = top_entry(25);
        std::vector<uint8_t> m25;
        if (m25_sz > 0 && disc.read_bytes(m25_lba, m25_sz, m25) &&
            m25.size() >= 0x499b4u + 360u * 8u) {
            auto b_entry = [&](int slot) -> std::pair<uint32_t, uint32_t> {
                size_t off = 0x499b4u + size_t(slot) * 8u;
                return {le32(m25.data() + off), le32(m25.data() + off + 4)};
            };

            /* 2a. Battle stages: battle[1..163] -> a0stg000..a0stg162 (LZS) */
            for (int stg = 0; stg < 163; ++stg) {
                auto [lba, sz] = b_entry(1 + stg);
                if (!lba || !sz || sz > 4u * 1024u * 1024u) continue;
                std::vector<uint8_t> comp, raw;
                if (!disc.read_bytes(lba, sz, comp) || !lzs_decompress(comp.data(), comp.size(), raw)) continue;
                auto tims = scan_embedded_tims(raw.data(), raw.size());
                char prefix[24];
                std::snprintf(prefix, sizeof(prefix), "a0stg%03d", stg);
                for (size_t ti = 0; ti < tims.size(); ++ti) {
                    add_tim_target(idx, std::string(prefix) + "-" + std::to_string(ti), tims[ti], true, false);
                }
            }

            /* 2b. Battle monsters: battle[167..310] -> c0m000..c0m143 (raw) */
            for (int m = 0; m < 144; ++m) {
                auto [lba, sz] = b_entry(167 + m);
                if (!lba || !sz || sz > 4u * 1024u * 1024u) continue;
                std::vector<uint8_t> raw;
                if (!disc.read_bytes(lba, sz, raw)) continue;
                auto tims = scan_embedded_tims(raw.data(), raw.size());
                char prefix[24];
                std::snprintf(prefix, sizeof(prefix), "c0m%03d", m);
                std::string bid(prefix);
                Ps1ModelInfo &binfo = idx.battle_models[bid];
                inspect_battle_dat_ps1(raw, binfo);
                if (binfo.tim_count == 0) binfo.tim_count = uint32_t(tims.size());
                for (size_t ti = 0; ti < tims.size(); ++ti) {
                    add_tim_target(idx, bid + "-" + std::to_string(ti), tims[ti], false, false,
                                   &binfo.vram_texture_hashes);
                }
                std::sort(binfo.vram_texture_hashes.begin(), binfo.vram_texture_hashes.end());
                binfo.vram_texture_hashes.erase(
                    std::unique(binfo.vram_texture_hashes.begin(), binfo.vram_texture_hashes.end()),
                    binfo.vram_texture_hashes.end());
            }

            /* 2c. Battle party characters & weapons: battle[311..359] (raw) */
            for (const auto &bc : kBattleChars) {
                auto [lba, sz] = b_entry(bc.slot);
                if (!lba || !sz || sz > 4u * 1024u * 1024u) continue;
                std::vector<uint8_t> raw;
                if (!disc.read_bytes(lba, sz, raw)) continue;
                auto tims = scan_embedded_tims(raw.data(), raw.size());
                std::string bid(bc.name);
                Ps1ModelInfo &binfo = idx.battle_models[bid];
                inspect_battle_dat_ps1(raw, binfo);
                if (binfo.tim_count == 0) binfo.tim_count = uint32_t(tims.size());
                for (size_t ti = 0; ti < tims.size(); ++ti) {
                    add_tim_target(idx, bid + "-" + std::to_string(ti), tims[ti], false, false,
                                   &binfo.vram_texture_hashes);
                }
                std::sort(binfo.vram_texture_hashes.begin(), binfo.vram_texture_hashes.end());
                binfo.vram_texture_hashes.erase(
                    std::unique(binfo.vram_texture_hashes.begin(), binfo.vram_texture_hashes.end()),
                    binfo.vram_texture_hashes.end());
            }
        }
    }

    /* 3. World map wmset.obj (module 26, LZS, table @ 0x2cca4, world[4]). */
    {
        auto [m26_lba, m26_sz] = top_entry(26);
        std::vector<uint8_t> m26_comp, m26;
        if (m26_sz > 0 && disc.read_bytes(m26_lba, m26_sz, m26_comp) &&
            lzs_decompress(m26_comp.data(), m26_comp.size(), m26) &&
            m26.size() >= 0x2cca4u + 12u * 8u) {
            uint32_t wm_lba = le32(m26.data() + 0x2cca4u + 4u * 8u);
            uint32_t wm_sz = le32(m26.data() + 0x2cca4u + 4u * 8u + 4u);
            std::vector<uint8_t> wm;
            if (wm_lba && wm_sz && wm_sz < 8u * 1024u * 1024u && disc.read_bytes(wm_lba, wm_sz, wm) &&
                wm.size() >= 48u * 4u) {
                for (int sec1 : {38, 39, 40, 42}) {
                    int s0 = sec1 - 1;
                    uint32_t start = le32(wm.data() + size_t(s0) * 4u);
                    uint32_t end = (s0 + 1 < 48) ? le32(wm.data() + size_t(s0 + 1) * 4u) : uint32_t(wm.size());
                    if (end <= start || end > wm.size()) continue;
                    auto tims = scan_embedded_tims(wm.data() + start, end - start);
                    Ps1ModelInfo &winfo = idx.world_models["wmset_section" + std::to_string(sec1)];
                    winfo.tim_count = uint32_t(tims.size());
                    for (size_t ti = 0; ti < tims.size(); ++ti) {
                        add_tim_target(idx, "wmset_section" + std::to_string(sec1) + "-" + std::to_string(ti),
                                       tims[ti], false, false, &winfo.vram_texture_hashes);
                        add_tim_target(idx, "section" + std::to_string(sec1) + "-" + std::to_string(ti),
                                       tims[ti], false, false);
                    }
                    std::sort(winfo.vram_texture_hashes.begin(), winfo.vram_texture_hashes.end());
                    winfo.vram_texture_hashes.erase(
                        std::unique(winfo.vram_texture_hashes.begin(), winfo.vram_texture_hashes.end()),
                        winfo.vram_texture_hashes.end());
                }
            }
        }
    }

    return true;
}

/* ---------- 7th Heaven / Junction VIII mod.xml Evaluator ---------- */

class ExprParser {
public:
    ExprParser(const std::string &s, const std::unordered_map<std::string, int> &vars)
        : s_(s), vars_(vars) {}

    bool eval() {
        int v = parse_or();
        return v != 0;
    }

private:
    void skip_ws() {
        while (pos_ < s_.size() && std::isspace(static_cast<unsigned char>(s_[pos_]))) ++pos_;
    }

    bool match_kw(const char *kw) {
        skip_ws();
        size_t len = std::strlen(kw);
        if (pos_ + len > s_.size()) return false;
        for (size_t i = 0; i < len; ++i) {
            if (std::tolower(static_cast<unsigned char>(s_[pos_ + i])) !=
                std::tolower(static_cast<unsigned char>(kw[i]))) return false;
        }
        if (pos_ + len < s_.size()) {
            unsigned char nx = static_cast<unsigned char>(s_[pos_ + len]);
            if (std::isalnum(nx) || nx == '_') return false;
        }
        pos_ += len;
        return true;
    }

    bool match_op(const char *op) {
        skip_ws();
        size_t len = std::strlen(op);
        if (pos_ + len > s_.size()) return false;
        if (s_.compare(pos_, len, op) != 0) return false;
        pos_ += len;
        return true;
    }

    int parse_or() {
        int lhs = parse_and();
        while (match_kw("or") || match_op("||")) {
            int rhs = parse_and();
            lhs = (lhs || rhs) ? 1 : 0;
        }
        return lhs;
    }

    int parse_and() {
        int lhs = parse_not();
        while (match_kw("and") || match_op("&&")) {
            int rhs = parse_not();
            lhs = (lhs && rhs) ? 1 : 0;
        }
        return lhs;
    }

    int parse_not() {
        if (match_kw("not") || match_op("!")) {
            return parse_not() ? 0 : 1;
        }
        return parse_cmp();
    }

    int parse_cmp() {
        int lhs = parse_atom();
        skip_ws();
        if (match_op("==")) return lhs == parse_atom() ? 1 : 0;
        if (match_op("!=")) return lhs != parse_atom() ? 1 : 0;
        if (match_op("<>")) return lhs != parse_atom() ? 1 : 0;
        if (match_op("<=")) return lhs <= parse_atom() ? 1 : 0;
        if (match_op(">=")) return lhs >= parse_atom() ? 1 : 0;
        if (match_op("="))  return lhs == parse_atom() ? 1 : 0;
        if (match_op("<"))  return lhs < parse_atom() ? 1 : 0;
        if (match_op(">"))  return lhs > parse_atom() ? 1 : 0;
        return lhs;
    }

    int parse_atom() {
        skip_ws();
        if (pos_ >= s_.size()) return 0;
        if (s_[pos_] == '(') {
            ++pos_;
            int v = parse_or();
            skip_ws();
            if (pos_ < s_.size() && s_[pos_] == ')') ++pos_;
            return v;
        }
        if (std::isdigit(static_cast<unsigned char>(s_[pos_])) ||
            (s_[pos_] == '-' && pos_ + 1 < s_.size() && std::isdigit(static_cast<unsigned char>(s_[pos_ + 1])))) {
            char *end = nullptr;
            long val = std::strtol(s_.c_str() + pos_, &end, 10);
            if (end && end > s_.c_str() + pos_) {
                pos_ = size_t(end - s_.c_str());
                return int(val);
            }
        }
        size_t start = pos_;
        while (pos_ < s_.size()) {
            unsigned char c = static_cast<unsigned char>(s_[pos_]);
            if (std::isalnum(c) || c == '_' || c == '-' || c == '.') ++pos_;
            else break;
        }
        std::string ident = to_lower(s_.substr(start, pos_ - start));
        if (ident == "true") return 1;
        if (ident == "false") return 0;
        auto it = vars_.find(ident);
        return (it != vars_.end()) ? it->second : 0;
    }

    const std::string &s_;
    const std::unordered_map<std::string, int> &vars_;
    size_t pos_ = 0;
};

std::string xml_unescape(std::string s) {
    auto replace_all = [&](const char *from, const char *to) {
        size_t p = 0;
        size_t fl = std::strlen(from), tl = std::strlen(to);
        while ((p = s.find(from, p)) != std::string::npos) {
            s.replace(p, fl, to);
            p += tl;
        }
    };
    replace_all("&amp;", "&");
    replace_all("&lt;", "<");
    replace_all("&gt;", ">");
    replace_all("&quot;", "\"");
    replace_all("&apos;", "'");
    return s;
}

bool extract_xml_attr(const std::string &tag, const std::string &attr_name, std::string &out) {
    std::string low_tag = to_lower(tag);
    std::string needle = to_lower(attr_name);
    size_t pos = 0;
    while ((pos = low_tag.find(needle, pos)) != std::string::npos) {
        if (pos > 0 && (std::isalnum(static_cast<unsigned char>(low_tag[pos - 1])) || low_tag[pos - 1] == '_')) {
            pos += needle.size();
            continue;
        }
        size_t p = pos + needle.size();
        while (p < tag.size() && std::isspace(static_cast<unsigned char>(tag[p]))) ++p;
        if (p >= tag.size() || tag[p] != '=') {
            pos += needle.size();
            continue;
        }
        ++p;
        while (p < tag.size() && std::isspace(static_cast<unsigned char>(tag[p]))) ++p;
        if (p >= tag.size() || (tag[p] != '"' && tag[p] != '\'')) return false;
        char q = tag[p++];
        size_t end = tag.find(q, p);
        if (end == std::string::npos) return false;
        out = xml_unescape(tag.substr(p, end - p));
        return true;
    }
    return false;
}

bool eval_active_when_block(const std::string &body, const std::unordered_map<std::string, int> &vars) {
    /* Evaluate <ActiveWhen> child XML: <And>, <Or>, <Not>, <Option>EXPR</Option>, <Condition>EXPR</Condition>. */
    std::string low = to_lower(body);
    bool is_or = (low.find("<or") != std::string::npos && low.find("<and") == std::string::npos);
    bool is_not = (low.find("<not") != std::string::npos);

    std::vector<bool> results;
    size_t pos = 0;
    while (pos < body.size()) {
        size_t o1 = low.find("<option", pos);
        size_t c1 = low.find("<condition", pos);
        size_t next = std::min(o1, c1);
        if (next == std::string::npos) break;
        bool is_opt = (next == o1);
        size_t gt = body.find('>', next);
        if (gt == std::string::npos) break;
        const char *close_tag = is_opt ? "</option>" : "</condition>";
        size_t cl = low.find(close_tag, gt + 1);
        if (cl == std::string::npos) break;
        std::string expr = trim(xml_unescape(body.substr(gt + 1, cl - (gt + 1))));
        if (!expr.empty()) {
            ExprParser ep(expr, vars);
            results.push_back(ep.eval());
        }
        pos = cl + std::strlen(close_tag);
    }

    if (results.empty()) {
        /* Plain text expression inside <ActiveWhen>...</ActiveWhen> */
        std::string plain = trim(xml_unescape(body));
        if (plain.empty() || plain.find('<') != std::string::npos) return false;
        ExprParser ep(plain, vars);
        return ep.eval();
    }

    bool agg = is_or ? false : true;
    for (bool r : results) {
        if (is_or) agg = agg || r;
        else agg = agg && r;
    }
    return is_not ? !agg : agg;
}

/* ---------- Mod Source Abstraction (IROS Archive or Unpacked Directory) ---------- */

struct ModFileRef {
    std::string rel_path; /* normalized lowercase-matched logical path inside mod */
    std::string orig_path;
    int source_idx = -1;
    uint64_t offset = 0;  /* for IROS archive */
    uint32_t size = 0;
    fs::path disk_path;   /* for unpacked file */
};

struct ModSource {
    std::string label;
    bool is_archive = false;
    fs::path archive_path;
    std::vector<ModFileRef> active_files;
};

bool read_mod_file(const ModSource &src, const ModFileRef &ref, std::vector<uint8_t> &out) {
    out.clear();
    if (src.is_archive) {
        std::ifstream in(src.archive_path, std::ios::binary);
        if (!in) return false;
        in.seekg(std::streamoff(ref.offset));
        out.resize(ref.size);
        in.read(reinterpret_cast<char *>(out.data()), std::streamsize(ref.size));
        return uint64_t(in.gcount()) == ref.size;
    }
    std::ifstream in(ref.disk_path, std::ios::binary | std::ios::ate);
    if (!in) return false;
    auto sz = in.tellg();
    if (sz < 0) return false;
    in.seekg(0);
    out.resize(size_t(sz));
    in.read(reinterpret_cast<char *>(out.data()), sz);
    return bool(in);
}

bool starts_with_folder(const std::string &low_path, const std::string &low_folder) {
    if (low_folder.empty()) return false;
    if (low_path.size() <= low_folder.size()) return false;
    return low_path.compare(0, low_folder.size(), low_folder) == 0 && low_path[low_folder.size()] == '/';
}

/* ---------- FFNx Texture Filename -> Canonical PS1 Key ---------- */

struct ParsedModTex {
    std::string canonical_key; /* e.g. "d000-0", "a0stg101-0", "c0m028-0", "wmset_section38-0" */
    int pal_idx = -1;          /* -1 if unspecified, or 0..255 */
    bool is_png = false;
};

bool parse_ffnx_tex_name(const std::string &rel_path, ParsedModTex &out) {
    std::string low = to_lower(rel_path);
    std::string ext;
    if (low.size() > 4 && low.substr(low.size() - 4) == ".dds") {
        ext = ".dds";
        out.is_png = false;
    } else if (low.size() > 4 && low.substr(low.size() - 4) == ".png") {
        ext = ".png";
        out.is_png = true;
    } else {
        return false;
    }
    if (low.find("preview/") != std::string::npos) return false;

    size_t slash = low.rfind('/');
    std::string fname = (slash == std::string::npos) ? low : low.substr(slash + 1);
    std::string stem = fname.substr(0, fname.size() - 4);
    while (!stem.empty() && stem.front() == '.') stem.erase(stem.begin());
    if (stem.empty()) return false;

    /* Pattern 1: wmset section textures: ".../section38_0_00" or "wmset_section38_0_00" or "section38-0_00" */
    {
        size_t sec_pos = stem.find("section");
        if (sec_pos != std::string::npos) {
            size_t p = sec_pos + 7;
            size_t d0 = p;
            while (p < stem.size() && std::isdigit(static_cast<unsigned char>(stem[p]))) ++p;
            if (p > d0 && p < stem.size() && (stem[p] == '_' || stem[p] == '-')) {
                std::string sec_num = stem.substr(d0, p - d0);
                ++p;
                size_t t0 = p;
                while (p < stem.size() && std::isdigit(static_cast<unsigned char>(stem[p]))) ++p;
                if (p > t0) {
                    int tim_idx = std::atoi(stem.substr(t0, p - t0).c_str());
                    out.canonical_key = "wmset_section" + sec_num + "-" + std::to_string(tim_idx);
                    out.pal_idx = -1;
                    if (p < stem.size() && (stem[p] == '_' || stem[p] == '-')) {
                        ++p;
                        if (p < stem.size() && std::isdigit(static_cast<unsigned char>(stem[p]))) {
                            out.pal_idx = std::atoi(stem.substr(p).c_str());
                        }
                    }
                    return true;
                }
            }
        }
    }

    /* Pattern 2: "<base>[.<ext>]-<tim_idx>[_<pal_idx>]" or "<base>[.<ext>]_<tim_idx>[_<pal_idx>]"
     * Covers:
     *   d000-0_00.dds, d000-1_00.dds
     *   A0STG101.X-0_00.dds, .A0STG030.X-0_01.dds, a0stg101_0_00.dds
     *   C0M028.DAT-0_00.dds, d0c000.dat-0_00.dds, d0w000.dat-0_00.dds */
    size_t dash = stem.rfind('-');
    size_t sep = dash;
    if (sep == std::string::npos) {
        /* Try underscore before tim index (e.g. a0stg101_0_00 -> base=a0stg101, tim=0, pal=0) */
        size_t u1 = stem.find('_');
        if (u1 != std::string::npos && u1 + 1 < stem.size() &&
            std::isdigit(static_cast<unsigned char>(stem[u1 + 1]))) {
            sep = u1;
        }
    }
    if (sep == std::string::npos || sep == 0 || sep + 1 >= stem.size()) return false;

    std::string base = stem.substr(0, sep);
    /* Strip embedded archive extension like ".x", ".dat", ".mch", ".tim" */
    size_t dot = base.find('.');
    if (dot != std::string::npos) base.resize(dot);
    if (base.empty()) return false;

    std::string rest = stem.substr(sep + 1);
    size_t p = 0;
    while (p < rest.size() && std::isdigit(static_cast<unsigned char>(rest[p]))) ++p;
    if (p == 0) return false;
    int tim_idx = std::atoi(rest.substr(0, p).c_str());
    int pal_idx = -1;
    if (p < rest.size() && (rest[p] == '_' || rest[p] == '-')) {
        size_t q = p + 1;
        size_t q0 = q;
        while (q < rest.size() && std::isdigit(static_cast<unsigned char>(rest[q]))) ++q;
        if (q > q0) pal_idx = std::atoi(rest.substr(q0, q - q0).c_str());
    }

    out.canonical_key = base + "-" + std::to_string(tim_idx);
    out.pal_idx = pal_idx;
    return true;
}

/* ---------- High-Poly .mch -> glTF 2.0 .glb Exporter ---------- */

struct MchTexProvider {
    /* Prefer textures from the SAME mod source as the .mch so UVs match 100%,
     * falling back to the global winning active texture if not in the same archive. */
    const std::unordered_map<std::string, std::pair<int, ModFileRef>> *same_src = nullptr;
    const std::unordered_map<std::string, std::pair<int, ModFileRef>> *global = nullptr;
    const std::vector<ModSource> *sources = nullptr;
};

struct PrimVert {
    float px, py, pz;
    float u, v;
    uint16_t joint;
};

struct PrimBucket {
    std::vector<PrimVert> verts;
    std::vector<uint32_t> indices;
};

bool pack_prim_buckets_to_glb(const std::map<uint32_t, PrimBucket> &buckets,
                              const std::vector<std::vector<uint8_t>> &embedded_pngs,
                              const fs::path &glb_out) {
    if (buckets.empty()) return false;

    std::vector<uint8_t> bin;
    json buffer_views = json::array();
    json accessors = json::array();
    json images = json::array();
    json textures = json::array();
    json materials = json::array();
    json primitives = json::array();

    auto align4 = [&]() {
        while (bin.size() & 3u) bin.push_back(0);
    };

    std::unordered_map<uint32_t, int> tex_to_mat;
    for (const auto &kv : buckets) {
        uint32_t ti = kv.first;
        json mat = {
            {"pbrMetallicRoughness", {
                {"metallicFactor", 0.0},
                {"roughnessFactor", 1.0}
            }},
            {"alphaMode", "MASK"},
            {"alphaCutoff", 0.5},
            {"doubleSided", true}
        };
        if (ti < embedded_pngs.size() && !embedded_pngs[ti].empty()) {
            align4();
            size_t bv_off = bin.size();
            const auto &png = embedded_pngs[ti];
            bin.insert(bin.end(), png.begin(), png.end());
            int bv_idx = int(buffer_views.size());
            buffer_views.push_back({
                {"buffer", 0},
                {"byteOffset", bv_off},
                {"byteLength", png.size()}
            });
            int img_idx = int(images.size());
            images.push_back({
                {"bufferView", bv_idx},
                {"mimeType", "image/png"}
            });
            int tex_idx = int(textures.size());
            textures.push_back({{"source", img_idx}});
            mat["pbrMetallicRoughness"]["baseColorTexture"] = {{"index", tex_idx}};
        }
        tex_to_mat[ti] = int(materials.size());
        materials.push_back(std::move(mat));
    }

    for (const auto &kv : buckets) {
        const auto &b = kv.second;
        if (b.verts.empty() || b.indices.empty()) continue;

        align4();
        size_t pos_off = bin.size();
        float min_p[3] = {1e9f, 1e9f, 1e9f};
        float max_p[3] = {-1e9f, -1e9f, -1e9f};
        for (const auto &v : b.verts) {
            write_f32(bin, v.px); write_f32(bin, v.py); write_f32(bin, v.pz);
            min_p[0] = std::min(min_p[0], v.px); max_p[0] = std::max(max_p[0], v.px);
            min_p[1] = std::min(min_p[1], v.py); max_p[1] = std::max(max_p[1], v.py);
            min_p[2] = std::min(min_p[2], v.pz); max_p[2] = std::max(max_p[2], v.pz);
        }
        int pos_bv = int(buffer_views.size());
        buffer_views.push_back({{"buffer", 0}, {"byteOffset", pos_off}, {"byteLength", b.verts.size() * 12u}});
        int pos_acc = int(accessors.size());
        accessors.push_back({
            {"bufferView", pos_bv}, {"componentType", 5126}, {"count", b.verts.size()}, {"type", "VEC3"},
            {"min", {min_p[0], min_p[1], min_p[2]}}, {"max", {max_p[0], max_p[1], max_p[2]}}
        });

        align4();
        size_t uv_off = bin.size();
        for (const auto &v : b.verts) {
            write_f32(bin, v.u); write_f32(bin, v.v);
        }
        int uv_bv = int(buffer_views.size());
        buffer_views.push_back({{"buffer", 0}, {"byteOffset", uv_off}, {"byteLength", b.verts.size() * 8u}});
        int uv_acc = int(accessors.size());
        accessors.push_back({
            {"bufferView", uv_bv}, {"componentType", 5126}, {"count", b.verts.size()}, {"type", "VEC2"}
        });

        align4();
        size_t j_off = bin.size();
        for (const auto &v : b.verts) {
            write_le16(bin, v.joint); write_le16(bin, 0); write_le16(bin, 0); write_le16(bin, 0);
        }
        int j_bv = int(buffer_views.size());
        buffer_views.push_back({{"buffer", 0}, {"byteOffset", j_off}, {"byteLength", b.verts.size() * 8u}});
        int j_acc = int(accessors.size());
        accessors.push_back({
            {"bufferView", j_bv}, {"componentType", 5123}, {"count", b.verts.size()}, {"type", "VEC4"}
        });

        align4();
        size_t w_off = bin.size();
        for (size_t k = 0; k < b.verts.size(); ++k) {
            write_f32(bin, 1.0f); write_f32(bin, 0.0f); write_f32(bin, 0.0f); write_f32(bin, 0.0f);
        }
        int w_bv = int(buffer_views.size());
        buffer_views.push_back({{"buffer", 0}, {"byteOffset", w_off}, {"byteLength", b.verts.size() * 16u}});
        int w_acc = int(accessors.size());
        accessors.push_back({
            {"bufferView", w_bv}, {"componentType", 5126}, {"count", b.verts.size()}, {"type", "VEC4"}
        });

        align4();
        size_t idx_off = bin.size();
        for (uint32_t i : b.indices) write_le32(bin, i);
        int idx_bv = int(buffer_views.size());
        buffer_views.push_back({{"buffer", 0}, {"byteOffset", idx_off}, {"byteLength", b.indices.size() * 4u}});
        int idx_acc = int(accessors.size());
        accessors.push_back({
            {"bufferView", idx_bv}, {"componentType", 5125}, {"count", b.indices.size()}, {"type", "SCALAR"}
        });

        primitives.push_back({
            {"attributes", {
                {"POSITION", pos_acc},
                {"TEXCOORD_0", uv_acc},
                {"JOINTS_0", j_acc},
                {"WEIGHTS_0", w_acc}
            }},
            {"indices", idx_acc},
            {"material", tex_to_mat[kv.first]}
        });
    }
    if (primitives.empty()) return false;

    align4();
    json gltf = {
        {"asset", {{"version", "2.0"}, {"generator", "Lionheart native FF8 mod importer"}}},
        {"scene", 0},
        {"scenes", json::array({{{"nodes", json::array({0})}}})},
        {"nodes", json::array({{{"mesh", 0}}})},
        {"meshes", json::array({{{"primitives", primitives}}})},
        {"materials", materials},
        {"buffers", json::array({{{"byteLength", bin.size()}}})},
        {"bufferViews", buffer_views},
        {"accessors", accessors}
    };
    if (!images.empty()) gltf["images"] = images;
    if (!textures.empty()) gltf["textures"] = textures;

    std::string jstr = gltf.dump();
    while (jstr.size() & 3u) jstr.push_back(' ');

    std::vector<uint8_t> glb;
    glb.reserve(12 + 8 + jstr.size() + 8 + bin.size());
    write_le32(glb, 0x46546C67u); /* "glTF" */
    write_le32(glb, 2u);
    write_le32(glb, uint32_t(12 + 8 + jstr.size() + 8 + bin.size()));
    write_le32(glb, uint32_t(jstr.size()));
    write_le32(glb, 0x4E4F534Au); /* "JSON" */
    glb.insert(glb.end(), jstr.begin(), jstr.end());
    write_le32(glb, uint32_t(bin.size()));
    write_le32(glb, 0x004E4942u); /* "BIN\0" */
    glb.insert(glb.end(), bin.begin(), bin.end());

    return write_file_bytes(glb_out, glb.data(), glb.size());
}

/* ---------- High-Poly Battle .dat -> glTF 2.0 .glb Exporter ---------- */

bool convert_battle_dat_to_glb(const std::string &model_id,
                               const std::vector<uint8_t> &dat,
                               const Ps1ModelInfo &ps1_info,
                               const MchTexProvider &tex_prov,
                               const fs::path &glb_out,
                               std::vector<int> &out_bone_remap) {
    if (dat.size() < 24) return false;
    uint32_t sec_count = le32(dat.data());
    if (sec_count < 3 || sec_count > 32 || 4u + (sec_count + 1u) * 4u > dat.size()) return false;

    uint32_t off_skel = le32(dat.data() + 4u + 0u * 4u);
    uint32_t off_geom = le32(dat.data() + 4u + 1u * 4u);
    uint32_t off_tex = le32(dat.data() + 4u + (sec_count - 1u) * 4u);
    if (off_skel >= dat.size() || off_geom + 4u > dat.size() || off_tex >= dat.size()) return false;

    /* 1. Skeleton bone table (Section 1: uint16 num_bones, uint16 scale, 12-byte pad, 48-byte bones). */
    uint16_t num_bones = 0;
    std::vector<int16_t> bone_parent;
    if (off_skel + 16u <= dat.size()) {
        num_bones = le16(dat.data() + off_skel);
        if (num_bones > 256) num_bones = 0;
        bone_parent.assign(num_bones, -1);
        for (uint16_t b = 0; b < num_bones; ++b) {
            size_t boff = off_skel + 16u + size_t(b) * 48u;
            if (boff + 2u <= dat.size()) bone_parent[b] = le_s16(dat.data() + boff);
        }
    }

    /* 2. Texture dimensions from Section N (TIM table) + HD texture lookup. */
    std::vector<uint32_t> tim_w, tim_h;
    std::vector<std::vector<uint8_t>> embedded_pngs;
    if (off_tex + 4u <= dat.size()) {
        uint32_t ntims = le32(dat.data() + off_tex);
        if (ntims > 0 && ntims <= 32 && off_tex + 4u + ntims * 4u <= dat.size()) {
            tim_w.assign(ntims, 128u);
            tim_h.assign(ntims, 128u);
            embedded_pngs.resize(ntims);
            for (uint32_t ti = 0; ti < ntims; ++ti) {
                uint32_t toff = off_tex + le32(dat.data() + off_tex + 4u + ti * 4u);
                TimParsed tp;
                if (toff < dat.size() && parse_tim_at(dat.data(), dat.size(), toff, tp)) {
                    tim_w[ti] = tp.px_w ? tp.px_w : 128u;
                    tim_h[ti] = tp.px_h ? tp.px_h : 128u;
                }
            }
        }
    }
    if (tim_w.empty()) {
        auto scanned = scan_embedded_tims(dat.data() + off_tex, dat.size() - off_tex);
        size_t ntims = std::max<size_t>(1, scanned.size());
        tim_w.assign(ntims, 128u);
        tim_h.assign(ntims, 128u);
        embedded_pngs.resize(ntims);
        for (size_t ti = 0; ti < scanned.size(); ++ti) {
            tim_w[ti] = scanned[ti].px_w ? scanned[ti].px_w : 128u;
            tim_h[ti] = scanned[ti].px_h ? scanned[ti].px_h : 128u;
        }
    }
    for (size_t ti = 0; ti < embedded_pngs.size(); ++ti) {
        std::string key = model_id + "-" + std::to_string(ti);
        const std::pair<int, ModFileRef> *found = nullptr;
        if (tex_prov.same_src) {
            auto it = tex_prov.same_src->find(key);
            if (it != tex_prov.same_src->end()) found = &it->second;
        }
        if (!found && tex_prov.global) {
            auto it = tex_prov.global->find(key);
            if (it != tex_prov.global->end()) found = &it->second;
        }
        if (found && tex_prov.sources && found->first >= 0 &&
            size_t(found->first) < tex_prov.sources->size()) {
            std::vector<uint8_t> raw;
            if (read_mod_file((*tex_prov.sources)[size_t(found->first)], found->second, raw)) {
                DdsImage img;
                if (decode_image_bytes(raw.data(), raw.size(), img)) {
                    encode_rgba_png(img.width, img.height, img.rgba.data(), embedded_pngs[ti]);
                }
            }
        }
    }

    /* 3. Geometry objects (Section 2). */
    uint32_t nobjs = le32(dat.data() + off_geom);
    if (nobjs == 0 || nobjs > 64 || off_geom + 4u + nobjs * 4u > dat.size()) return false;

    std::map<uint32_t, PrimBucket> buckets;
    std::vector<uint16_t> used_bones;
    std::unordered_map<uint16_t, uint16_t> bone_to_joint;
    auto joint_for_bone = [&](uint16_t bid) -> uint16_t {
        auto it = bone_to_joint.find(bid);
        if (it != bone_to_joint.end()) return it->second;
        uint16_t j = uint16_t(used_bones.size());
        bone_to_joint[bid] = j;
        used_bones.push_back(bid);
        return j;
    };

    for (uint32_t oi = 0; oi < nobjs; ++oi) {
        uint32_t oo = off_geom + le32(dat.data() + off_geom + 4u + oi * 4u);
        if (oo + 2u > dat.size()) continue;
        uint16_t ngroups = le16(dat.data() + oo);
        size_t p = oo + 2u;

        struct BattleVert { float x, y, z; uint16_t joint; };
        std::vector<BattleVert> verts;
        for (uint16_t g = 0; g < ngroups; ++g) {
            if (p + 4u > dat.size()) break;
            uint16_t bid = le16(dat.data() + p + 0u);
            uint16_t vc = le16(dat.data() + p + 2u);
            p += 4u;
            if (p + size_t(vc) * 6u > dat.size()) break;
            uint16_t j = joint_for_bone(bid);
            for (uint16_t vi = 0; vi < vc; ++vi) {
                const uint8_t *vp = dat.data() + p + size_t(vi) * 6u;
                verts.push_back({float(le_s16(vp + 0)), float(le_s16(vp + 2)), float(le_s16(vp + 4)), j});
            }
            p += size_t(vc) * 6u;
        }
        if (verts.empty()) continue;

        /* Align relative to object start (4-byte boundary) before triangle/quad header. */
        size_t rel = p - oo;
        if (rel & 3u) p += (4u - (rel & 3u));
        if (p + 12u > dat.size()) continue;

        uint16_t ntris = le16(dat.data() + p + 0u);
        uint16_t nquads = le16(dat.data() + p + 2u);
        p += 12u; /* 2+2 + 8 bytes padding */

        auto emit = [&](uint32_t tex_id, uint16_t vi, uint8_t u, uint8_t v) -> uint32_t {
            if (tex_id >= tim_w.size()) tex_id = 0;
            if (vi >= verts.size()) vi = 0;
            auto &b = buckets[tex_id];
            PrimVert pv{};
            pv.px = verts[vi].x; pv.py = verts[vi].y; pv.pz = verts[vi].z;
            pv.u = (float(u) + 0.5f) / float(tim_w[tex_id]);
            pv.v = (float(v) + 0.5f) / float(tim_h[tex_id]);
            pv.joint = verts[vi].joint;
            uint32_t idx_out = uint32_t(b.verts.size());
            b.verts.push_back(pv);
            return idx_out;
        };

        for (uint16_t ti = 0; ti < ntris && p + 16u <= dat.size(); ++ti, p += 16u) {
            const uint8_t *tp = dat.data() + p;
            uint16_t v0 = le16(tp + 0) & 0x0FFFu;
            uint16_t v1 = le16(tp + 2) & 0x0FFFu;
            uint16_t v2 = le16(tp + 4) & 0x0FFFu;
            uint16_t tex_id = le16(tp + 12) & 0x000Fu;
            if (tex_id >= tim_w.size()) tex_id = 0;
            uint32_t i0 = emit(tex_id, v0, tp[6], tp[7]);
            uint32_t i1 = emit(tex_id, v1, tp[8], tp[9]);
            uint32_t i2 = emit(tex_id, v2, tp[10], tp[11]);
            auto &b = buckets[tex_id];
            b.indices.push_back(i0); b.indices.push_back(i1); b.indices.push_back(i2);
        }
        for (uint16_t qi = 0; qi < nquads && p + 20u <= dat.size(); ++qi, p += 20u) {
            const uint8_t *qp = dat.data() + p;
            uint16_t v0 = le16(qp + 0) & 0x0FFFu;
            uint16_t v1 = le16(qp + 2) & 0x0FFFu;
            uint16_t v2 = le16(qp + 4) & 0x0FFFu;
            uint16_t v3 = le16(qp + 6) & 0x0FFFu;
            uint16_t tex_id = le16(qp + 16) & 0x000Fu;
            if (tex_id >= tim_w.size()) tex_id = 0;
            uint32_t i0 = emit(tex_id, v0, qp[8], qp[9]);
            uint32_t i1 = emit(tex_id, v1, qp[10], qp[11]);
            uint32_t i2 = emit(tex_id, v2, qp[12], qp[13]);
            uint32_t i3 = emit(tex_id, v3, qp[14], qp[15]);
            auto &b = buckets[tex_id];
            b.indices.push_back(i0); b.indices.push_back(i1); b.indices.push_back(i2);
            b.indices.push_back(i1); b.indices.push_back(i3); b.indices.push_back(i2);
        }
    }

    if (buckets.empty()) return false;

    out_bone_remap.clear();
    if (!used_bones.empty()) {
        out_bone_remap.resize(used_bones.size(), 0);
        size_t ps1_bones = ps1_info.skin_bones.size();
        for (size_t j = 0; j < used_bones.size(); ++j) {
            uint16_t b = used_bones[j];
            if (ps1_bones > 0) {
                int16_t cur = int16_t(b);
                int mapped = -1;
                for (int depth = 0; depth < 32 && cur >= 0; ++depth) {
                    if (size_t(cur) < ps1_bones) { mapped = int(cur); break; }
                    if (size_t(cur) < bone_parent.size() && bone_parent[size_t(cur)] != cur)
                        cur = bone_parent[size_t(cur)];
                    else break;
                }
                out_bone_remap[j] = mapped >= 0 ? mapped : 0;
            } else {
                out_bone_remap[j] = int(b);
            }
        }
    }

    return pack_prim_buckets_to_glb(buckets, embedded_pngs, glb_out);
}

bool convert_mch_to_glb(const std::string &model_id,
                        const std::vector<uint8_t> &mch,
                        const Ps1ModelInfo &ps1_info,
                        const MchTexProvider &tex_prov,
                        const fs::path &glb_out,
                        std::vector<int> &out_bone_remap) {
    if (mch.size() < 64) return false;

    std::vector<uint32_t> tim_offs;
    size_t pos = 0;
    while (pos + 4 <= mch.size()) {
        uint32_t v = le32(mch.data() + pos);
        pos += 4;
        if (v == 0xFFFFFFFFu) break;
        tim_offs.push_back(v & 0xFFFFFFu);
    }
    if (pos + 4 > mch.size()) return false;
    uint32_t moff = le32(mch.data() + pos);
    if (moff + 64 > mch.size()) return false;

    uint32_t c_bones = le32(mch.data() + moff + 0 * 4);
    uint32_t c_verts = le32(mch.data() + moff + 1 * 4);
    uint32_t c_faces = le32(mch.data() + moff + 3 * 4);
    uint32_t c_skin = le32(mch.data() + moff + 5 * 4);
    uint32_t off_bones = le32(mch.data() + moff + 8 * 4);
    uint32_t off_verts = le32(mch.data() + moff + 9 * 4);
    uint32_t off_faces = le32(mch.data() + moff + 11 * 4);
    uint32_t off_skin = le32(mch.data() + moff + 13 * 4);

    if (c_verts == 0 || c_faces == 0 || c_skin == 0 || c_verts > 65535 || c_faces > 100000 || c_skin > 128)
        return false;
    if (moff + off_verts + size_t(c_verts) * 8u > mch.size()) return false;
    if (moff + off_faces + size_t(c_faces) * 64u > mch.size()) return false;
    if (moff + off_skin + size_t(c_skin) * 8u > mch.size()) return false;

    /* Bone parent table (1-based bone_id -> 1-based parent bone_id, 0 = root). */
    std::vector<uint16_t> bone_parent;
    if (c_bones <= 256 && moff + off_bones + size_t(c_bones) * 64u <= mch.size()) {
        bone_parent.resize(c_bones + 1u, 0);
        for (uint32_t b = 0; b < c_bones; ++b) {
            bone_parent[b + 1u] = le16(mch.data() + moff + off_bones + size_t(b) * 64u);
        }
    }

    /* 1. Build vertex -> mod_skin_idx table and compute exact bone_remap against PS1 .mch. */
    std::vector<uint16_t> vert_skin(c_verts, 0);
    std::vector<uint16_t> mod_skin_bones(c_skin, 0);
    for (uint32_t s = 0; s < c_skin; ++s) {
        const uint8_t *sp = mch.data() + moff + off_skin + size_t(s) * 8u;
        uint16_t v0 = le16(sp + 0);
        uint16_t vc = le16(sp + 2);
        uint16_t bid = le16(sp + 4);
        mod_skin_bones[s] = bid;
        for (uint32_t k = 0; k < vc; ++k) {
            if (uint32_t(v0) + k < c_verts) vert_skin[v0 + k] = uint16_t(s);
        }
    }

    out_bone_remap.clear();
    if (!ps1_info.skin_bones.empty()) {
        std::unordered_map<uint16_t, int> ps1_bone_to_run;
        for (size_t i = 0; i < ps1_info.skin_bones.size(); ++i) {
            if (!ps1_bone_to_run.count(ps1_info.skin_bones[i])) {
                ps1_bone_to_run[ps1_info.skin_bones[i]] = int(i);
            }
        }
        out_bone_remap.resize(c_skin, 0);
        for (uint32_t s = 0; s < c_skin; ++s) {
            uint16_t cur = mod_skin_bones[s];
            int mapped = -1;
            for (int depth = 0; depth < 32 && cur > 0; ++depth) {
                auto it = ps1_bone_to_run.find(cur);
                if (it != ps1_bone_to_run.end()) {
                    mapped = it->second;
                    break;
                }
                if (cur < bone_parent.size() && bone_parent[cur] != cur) {
                    cur = bone_parent[cur];
                } else {
                    break;
                }
            }
            out_bone_remap[s] = (mapped >= 0) ? mapped : 0;
        }
    }

    /* 2. Collect embedded TIM dimensions & decode HD PNG bytes for each texture slot. */
    size_t tex_count = std::max<size_t>(1, tim_offs.size());
    std::vector<uint32_t> tim_w(tex_count, 128), tim_h(tex_count, 128);
    std::vector<std::vector<uint8_t>> embedded_pngs(tex_count);
    for (size_t ti = 0; ti < tim_offs.size(); ++ti) {
        TimParsed tp;
        if (parse_tim_at(mch.data(), mch.size(), tim_offs[ti], tp)) {
            tim_w[ti] = tp.px_w ? tp.px_w : 128u;
            tim_h[ti] = tp.px_h ? tp.px_h : 128u;
        }
        std::string key = model_id + "-" + std::to_string(ti);
        const std::pair<int, ModFileRef> *found = nullptr;
        if (tex_prov.same_src) {
            auto it = tex_prov.same_src->find(key);
            if (it != tex_prov.same_src->end()) found = &it->second;
        }
        if (!found && tex_prov.global) {
            auto it = tex_prov.global->find(key);
            if (it != tex_prov.global->end()) found = &it->second;
        }
        if (found && tex_prov.sources && found->first >= 0 &&
            size_t(found->first) < tex_prov.sources->size()) {
            std::vector<uint8_t> raw;
            if (read_mod_file((*tex_prov.sources)[size_t(found->first)], found->second, raw)) {
                DdsImage img;
                if (decode_image_bytes(raw.data(), raw.size(), img)) {
                    encode_rgba_png(img.width, img.height, img.rgba.data(), embedded_pngs[ti]);
                }
            }
        }
    }

    /* 3. Build per-texture primitive geometry. */
    std::map<uint32_t, PrimBucket> buckets;

    auto read_pos = [&](uint16_t vi, float &x, float &y, float &z) {
        const uint8_t *vp = mch.data() + moff + off_verts + size_t(vi) * 8u;
        x = float(le_s16(vp + 0));
        y = float(le_s16(vp + 2));
        z = float(le_s16(vp + 4));
    };

    for (uint32_t fi = 0; fi < c_faces; ++fi) {
        const uint8_t *fp = mch.data() + moff + off_faces + size_t(fi) * 64u;
        uint32_t poly_id = le32(fp + 0);
        bool is_quad = ((poly_id >> 24) == 0x2Du);
        uint16_t vs[4] = {le16(fp + 12), le16(fp + 14), le16(fp + 16), le16(fp + 18)};
        uint8_t uvs[4][2] = {
            {fp[44], fp[45]}, {fp[46], fp[47]}, {fp[48], fp[49]}, {fp[50], fp[51]}
        };
        uint16_t tex_id = le16(fp + 54);
        if (tex_id >= tex_count) tex_id = 0;

        float inv_w = 1.0f / float(tim_w[tex_id]);
        float inv_h = 1.0f / float(tim_h[tex_id]);
        auto &b = buckets[tex_id];

        auto emit_vert = [&](int corner) -> uint32_t {
            uint16_t vi = vs[corner];
            if (vi >= c_verts) vi = 0;
            PrimVert pv{};
            read_pos(vi, pv.px, pv.py, pv.pz);
            pv.u = (float(uvs[corner][0]) + 0.5f) * inv_w;
            pv.v = (float(uvs[corner][1]) + 0.5f) * inv_h;
            pv.joint = vert_skin[vi];
            uint32_t idx_out = uint32_t(b.verts.size());
            b.verts.push_back(pv);
            return idx_out;
        };

        if (is_quad) {
            uint32_t i0 = emit_vert(0), i1 = emit_vert(1), i2 = emit_vert(2), i3 = emit_vert(3);
            /* PS1 PolyFT4 Z-order quad: (0, 1, 2) + (1, 3, 2) */
            b.indices.push_back(i0); b.indices.push_back(i1); b.indices.push_back(i2);
            b.indices.push_back(i1); b.indices.push_back(i3); b.indices.push_back(i2);
        } else {
            uint32_t i0 = emit_vert(0), i1 = emit_vert(1), i2 = emit_vert(2);
            b.indices.push_back(i0); b.indices.push_back(i1); b.indices.push_back(i2);
        }
    }

    return pack_prim_buckets_to_glb(buckets, embedded_pngs, glb_out);
}

/* ---------- Progress Helper ---------- */

struct ProgressSink {
    std::mutex mu;
    host_mod_import_status_t st{};

    void set_stage(const char *name) {
        std::lock_guard<std::mutex> lk(mu);
        std::snprintf(st.stage, sizeof(st.stage), "%s", name ? name : "");
    }
    void set_total(unsigned total) {
        std::lock_guard<std::mutex> lk(mu);
        st.total = total;
    }
    void inc_done() {
        std::lock_guard<std::mutex> lk(mu);
        ++st.done;
    }
};

bool is_safe_rel_path(const std::string &rel) {
    if (rel.empty() || rel[0] == '/' || rel[0] == '\\') return false;
    if (rel.find("..") != std::string::npos) return false;
    if (rel.size() >= 2 && rel[1] == ':') return false;
    return true;
}

std::string slugify_mod_id(const std::string &raw) {
    std::string out;
    out.reserve(48);
    bool last_dash = false;
    for (unsigned char c : raw) {
        if (std::isalnum(c)) {
            out.push_back(char(std::tolower(c)));
            last_dash = false;
        } else if (!out.empty() && !last_dash) {
            out.push_back('-');
            last_dash = true;
        }
        if (out.size() >= 52) break;
    }
    while (!out.empty() && out.back() == '-') out.pop_back();
    if (out.empty()) out = "mod";
    return out;
}

bool path_occupied(const fs::path &path) {
    std::error_code ec;
    const auto status = fs::symlink_status(path, ec);
    return status.type() != fs::file_type::not_found && ec != std::errc::no_such_file_or_directory;
}

std::string asset_root(const fs::path &path) {
    if (path.empty()) return {};
    std::error_code ec;
    fs::path absolute = fs::absolute(path, ec);
    if (ec) return {};
    fs::path root = fs::weakly_canonical(absolute, ec);
    return ec ? std::string{} : root.generic_string();
}

bool move_file_safe(const fs::path &src, const fs::path &dst) {
    std::error_code ec;
    if (!path_occupied(src)) return true;
    if (!fs::is_regular_file(fs::symlink_status(src, ec))) return false;
    fs::create_directories(dst.parent_path(), ec);
    if (ec || !fs::copy_file(src, dst, ec)) return false;
    if (fs::remove(src, ec)) return true;
    fs::remove(dst, ec);
    return false;
}

bool load_registry_json(const fs::path &reg_path, json &out) {
    out = {{"version", 1}, {"mods", json::array()}};
    std::error_code ec;
    if (!fs::is_regular_file(reg_path, ec)) return false;
    std::ifstream in(reg_path);
    if (!in) return false;
    try {
        json parsed = json::parse(in);
        if (parsed.is_object() && parsed.contains("mods") && parsed["mods"].is_array()) {
            out = std::move(parsed);
            return true;
        }
    } catch (...) {}
    return false;
}

bool save_registry_json(const fs::path &reg_path, const json &doc) {
    std::error_code ec;
    fs::create_directories(reg_path.parent_path(), ec);
    fs::path tmp = reg_path;
    tmp += ".tmp";
    std::string dump = doc.dump(2) + "\n";
    if (!write_file_bytes(tmp, reinterpret_cast<const uint8_t *>(dump.data()), dump.size())) return false;
    fs::rename(tmp, reg_path, ec);
    if (!ec) return true;
    fs::remove(tmp, ec);
    return false;
}

bool read_mod_xml_from_path(const std::string &mod_path, std::string &xml_out, std::string &label_out) {
    xml_out.clear();
    label_out.clear();
    std::error_code ec;
    fs::path p(mod_path);
    auto try_archive = [&](const fs::path &ap) -> bool {
        std::vector<IrosEntry> entries;
        if (!parse_iros_directory(ap.string(), entries)) return false;
        for (const auto &e : entries) {
            if (e.flags != 0 || e.size == 0 || e.size >= 4u * 1024u * 1024u) continue;
            std::string norm = normalize_slashes(e.name);
            if (to_lower(norm) == "mod.xml") {
                std::ifstream in(ap, std::ios::binary);
                if (!in) return false;
                in.seekg(std::streamoff(e.offset));
                std::string buf(e.size, '\0');
                in.read(&buf[0], std::streamsize(e.size));
                if (uint64_t(in.gcount()) == e.size) {
                    xml_out = std::move(buf);
                    label_out = ap.filename().string();
                    return true;
                }
            }
        }
        return false;
    };
    if (fs::is_regular_file(p, ec)) {
        std::string ext = to_lower(p.extension().string());
        if (ext == ".iroj" || ext == ".iro") return try_archive(p);
        return false;
    }
    if (fs::is_directory(p, ec)) {
        fs::path direct = p / "mod.xml";
        if (fs::is_regular_file(direct, ec)) {
            std::ifstream in(direct, std::ios::binary);
            if (in) {
                xml_out.assign((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
                label_out = p.filename().string();
                return !xml_out.empty();
            }
        }
        std::vector<fs::path> archives;
        for (const auto &ent : fs::directory_iterator(p, ec)) {
            if (!ent.is_regular_file(ec)) continue;
            std::string ext = to_lower(ent.path().extension().string());
            if (ext == ".iroj" || ext == ".iro") archives.push_back(ent.path());
        }
        std::sort(archives.begin(), archives.end(), [](const fs::path &a, const fs::path &b) {
            return to_lower(a.filename().string()) < to_lower(b.filename().string());
        });
        for (const auto &ap : archives) {
            if (try_archive(ap)) return true;
        }
    }
    return false;
}

bool is_audio_pack_file(const std::string &low_path, std::string &out_rel) {
    static const char *const kExts[] = {".ogg", ".wav", ".flac", ".mp3"};
    bool ext_ok = false;
    for (const char *ext : kExts) {
        size_t el = std::strlen(ext);
        if (low_path.size() > el && low_path.compare(low_path.size() - el, el, ext) == 0) {
            ext_ok = true;
            break;
        }
    }
    if (!ext_ok) return false;
    if (low_path.find("preview/") != std::string::npos) return false;
    /* Preserve clean relative subpath starting at music/, sfx/, audio/, or vgmstream/. */
    static const char *const kRoots[] = {"music/", "sfx/", "audio/", "vgmstream/"};
    for (const char *root : kRoots) {
        size_t pos = low_path.find(root);
        if (pos != std::string::npos) {
            out_rel = low_path.substr(pos);
            return is_safe_rel_path(out_rel);
        }
    }
    size_t sl = low_path.rfind('/');
    out_rel = (sl == std::string::npos) ? low_path : low_path.substr(sl + 1);
    return is_safe_rel_path(out_rel);
}

bool run_import_impl(const std::vector<std::string> &disc_paths,
                     const std::vector<std::string> &mod_paths,
                     const std::string &textures_out,
                     const std::string &models_out,
                     const std::string &movies_out,
                     const std::string &audio_out,
                     const std::string &registry_dir,
                     const std::vector<std::pair<std::string, int>> &overrides,
                     ProgressSink *sink,
                     host_mod_import_status_t &final_status) {
    final_status = {};
    final_status.running = 1;
    std::vector<std::string> log_lines;
    unsigned warnings = 0;
    auto add_log = [&](const std::string &msg, bool is_warn = false) {
        if (is_warn) ++warnings;
        if (log_lines.size() < 48) log_lines.push_back((is_warn ? "[WARN] " : "") + msg);
    };

    if (disc_paths.empty()) {
        std::snprintf(final_status.error, sizeof(final_status.error),
                      "No FF8 PS1 disc configured. Add Disc 1 first.");
        final_status.running = 0;
        final_status.finished = 1;
        return false;
    }
    if (mod_paths.empty()) {
        std::snprintf(final_status.error, sizeof(final_status.error),
                      "No mod archive or folder specified.");
        final_status.running = 0;
        final_status.finished = 1;
        return false;
    }

    const bool managed = !registry_dir.empty();
    const fs::path reg_path = fs::path(registry_dir) / "mods-registry.json";
    json registry;
    json roots = json::object();
    if (managed) {
        if (!load_registry_json(reg_path, registry) && path_occupied(reg_path)) {
            std::snprintf(final_status.error, sizeof(final_status.error), "Could not read the mod registry.");
            final_status.running = 0;
            final_status.finished = 1;
            return false;
        }
        roots = {{"textures", asset_root(textures_out)}, {"models", asset_root(models_out)},
                 {"movies", asset_root(movies_out)}, {"audio", asset_root(audio_out)}};
    }
    std::atomic<unsigned> conflicts{0};
    auto occupied_output = [&](const fs::path &path) {
        if (!managed || !path_occupied(path)) return false;
        ++conflicts;
        return true;
    };

    /* Step 1: Index PS1 FF8 disc(s). */
    if (sink) sink->set_stage("Indexing FF8 PlayStation disc...");
    Ps1Index ps1;
    std::string err;
    unsigned indexed_discs = 0;
    for (const auto &dp : disc_paths) {
        if (dp.empty()) continue;
        std::string disc_err;
        if (index_ps1_disc(dp, ps1, &disc_err)) {
            ++indexed_discs;
        } else if (err.empty()) {
            err = disc_err;
        }
    }
    if (indexed_discs == 0) {
        std::snprintf(final_status.error, sizeof(final_status.error), "%s",
                      err.empty() ? "Failed to read FF8 PS1 disc." : err.c_str());
        final_status.running = 0;
        final_status.finished = 1;
        return false;
    }

    /* Step 2: Discover mod sources (.iroj/.iro archives and unpacked directories). */
    if (sink) sink->set_stage("Scanning FF8 mod packs...");
    std::vector<fs::path> archive_paths;
    std::vector<fs::path> unpacked_dirs;
    for (const auto &mp : mod_paths) {
        std::error_code ec;
        fs::path p(mp);
        if (fs::is_regular_file(p, ec)) {
            std::string ext = to_lower(p.extension().string());
            if (ext == ".iroj" || ext == ".iro") archive_paths.push_back(p);
        } else if (fs::is_directory(p, ec)) {
            bool found_archive = false;
            for (const auto &ent : fs::directory_iterator(p, ec)) {
                if (!ent.is_regular_file(ec)) continue;
                std::string ext = to_lower(ent.path().extension().string());
                if (ext == ".iroj" || ext == ".iro") {
                    archive_paths.push_back(ent.path());
                    found_archive = true;
                }
            }
            if (!found_archive) unpacked_dirs.push_back(p);
        }
    }
    std::sort(archive_paths.begin(), archive_paths.end(), [](const fs::path &a, const fs::path &b) {
        return to_lower(a.filename().string()) < to_lower(b.filename().string());
    });

    std::vector<ModSource> sources;
    std::string primary_mod_name;
    auto filter_entries_with_xml = [&](const std::string &src_label,
                                       const std::string &xml_text,
                                       const std::vector<ModFileRef> &all_refs) -> std::vector<ModFileRef> {
        if (!xml_text.empty() && primary_mod_name.empty()) {
            host_mod_xml_config_t cfg{};
            if (parse_mod_xml_config(xml_text, cfg) && cfg.mod_name[0]) primary_mod_name = cfg.mod_name;
        }
        ModXmlResult xr = xml_text.empty() ? ModXmlResult{} : parse_and_eval_mod_xml(xml_text, overrides);
        if (xr.has_folder_rules) {
            std::string summary = src_label + ": mod.xml active folders (" +
                                  std::to_string(xr.active_folders.size()) + "/" +
                                  std::to_string(xr.declared_folders.size()) + ")";
            add_log(summary);
        }
        std::vector<ModFileRef> active;
        for (const auto &r : all_refs) {
            std::string low = to_lower(r.rel_path);
            if (low == "mod.xml" || starts_with_folder(low, "preview")) continue;
            if (xr.has_folder_rules) {
                bool matched_active = false;
                for (const auto &af : xr.active_folders) {
                    if (starts_with_folder(low, af)) { matched_active = true; break; }
                }
                if (matched_active) {
                    active.push_back(r);
                    continue;
                }
                bool matched_declared = false;
                for (const auto &df : xr.declared_folders) {
                    if (starts_with_folder(low, df)) { matched_declared = true; break; }
                }
                if (matched_declared) continue; /* explicitly inactive ModFolder */
            }
            active.push_back(r);
        }
        return active;
    };

    for (const auto &ap : archive_paths) {
        std::vector<IrosEntry> entries;
        std::string aerr;
        if (!parse_iros_directory(ap.string(), entries, &aerr)) {
            add_log(ap.filename().string() + ": skipped (" + (aerr.empty() ? "invalid IROS" : aerr) + ")", true);
            continue;
        }
        ModSource src;
        src.label = ap.filename().string();
        src.is_archive = true;
        src.archive_path = ap;
        int src_idx = int(sources.size());

        std::string xml_text;
        std::vector<ModFileRef> all_refs;
        all_refs.reserve(entries.size());
        unsigned compressed_skipped = 0;
        for (const auto &e : entries) {
            if (e.flags != 0) { ++compressed_skipped; continue; } /* skip unsupported compressed entries */
            std::string norm = normalize_slashes(e.name);
            ModFileRef r;
            r.rel_path = norm;
            r.orig_path = e.name;
            r.source_idx = src_idx;
            r.offset = e.offset;
            r.size = e.size;
            if (to_lower(norm) == "mod.xml" && e.size < 4u * 1024u * 1024u) {
                std::vector<uint8_t> xb;
                if (read_mod_file(src, r, xb)) {
                    xml_text.assign(reinterpret_cast<const char *>(xb.data()), xb.size());
                }
            }
            all_refs.push_back(std::move(r));
        }
        if (compressed_skipped > 0) {
            add_log(src.label + ": skipped " + std::to_string(compressed_skipped) + " compressed IROS entries", true);
        }
        src.active_files = filter_entries_with_xml(src.label, xml_text, all_refs);
        sources.push_back(std::move(src));
    }

    for (const auto &dir : unpacked_dirs) {
        ModSource src;
        src.label = dir.filename().string();
        src.is_archive = false;
        int src_idx = int(sources.size());
        std::string xml_text;
        std::vector<ModFileRef> all_refs;
        std::error_code ec;
        for (const auto &ent : fs::recursive_directory_iterator(dir, ec)) {
            if (!ent.is_regular_file(ec)) continue;
            fs::path rel = ent.path().lexically_relative(dir);
            std::string norm = normalize_slashes(rel.generic_string());
            ModFileRef r;
            r.rel_path = norm;
            r.orig_path = norm;
            r.source_idx = src_idx;
            r.disk_path = ent.path();
            r.size = uint32_t(ent.file_size(ec));
            if (to_lower(norm) == "mod.xml" && r.size < 4u * 1024u * 1024u) {
                std::vector<uint8_t> xb;
                if (read_mod_file(src, r, xb)) {
                    xml_text.assign(reinterpret_cast<const char *>(xb.data()), xb.size());
                }
            }
            all_refs.push_back(std::move(r));
        }
        src.active_files = filter_entries_with_xml(src.label, xml_text, all_refs);
        sources.push_back(std::move(src));
    }

    if (sources.empty()) {
        std::snprintf(final_status.error, sizeof(final_status.error),
                      "No .iroj/.iro archives or mod files found.");
        final_status.running = 0;
        final_status.finished = 1;
        return false;
    }

    /* Step 3: Collect winning active textures, field .mch models, and optional movies. */
    struct TexChoice {
        int source_idx = -1;
        ModFileRef ref;
        int pal_idx = -1;
        std::string key;
    };
    /* Key: canonical_key + "#" + pal_idx */
    std::unordered_map<std::string, TexChoice> tex_choices;
    /* Per-source texture map (canonical_key -> first/pal-0 file) for exact .mch pairing */
    std::vector<std::unordered_map<std::string, std::pair<int, ModFileRef>>> per_src_tex(sources.size());
    std::unordered_map<std::string, std::pair<int, ModFileRef>> global_mch_tex;

    enum class ModelKind { FieldMch, BattleDat, WorldMch, PrebuiltGlb };
    struct ModelChoice {
        int source_idx = -1;
        ModFileRef ref;
        ModelKind kind = ModelKind::FieldMch;
        std::string category; /* "field", "battle", "world" */
        std::string model_id; /* "d000", "c0m001", "d0c000", "d0w000", "squall", "ragnarok", etc. */
    };
    std::unordered_map<std::string, ModelChoice> model_choices;

    struct MovieChoice {
        int source_idx = -1;
        ModFileRef ref;
        std::string filename;
    };
    std::unordered_map<std::string, MovieChoice> movie_choices;

    struct AudioChoice {
        int source_idx = -1;
        ModFileRef ref;
        std::string rel_path;
    };
    std::unordered_map<std::string, AudioChoice> audio_choices;

    for (size_t si = 0; si < sources.size(); ++si) {
        for (const auto &ref : sources[si].active_files) {
            std::string low = to_lower(ref.rel_path);
            size_t sl = low.rfind('/');
            std::string fname = (sl == std::string::npos) ? low : low.substr(sl + 1);

            /* Field main character .mch model */
            if (fname.size() == 8 && fname[0] == 'd' &&
                std::isdigit(static_cast<unsigned char>(fname[1])) &&
                std::isdigit(static_cast<unsigned char>(fname[2])) &&
                std::isdigit(static_cast<unsigned char>(fname[3])) &&
                fname.substr(4) == ".mch") {
                std::string mid = fname.substr(0, 4);
                if (ps1.field_models.count(mid)) {
                    model_choices["field/" + mid] = {int(si), ref, ModelKind::FieldMch, "field", mid};
                }
                continue;
            }

            /* Battle character, weapon, and enemy .dat models (c0mNNN.dat, dXcNNN.dat, dXwNNN.dat) */
            if (fname.size() == 10 && fname.substr(6) == ".dat") {
                std::string bid = fname.substr(0, 6);
                if (ps1.battle_models.count(bid)) {
                    model_choices["battle/" + bid] = {int(si), ref, ModelKind::BattleDat, "battle", bid};
                    continue;
                }
            }

            /* World map .mch models (c0wNNN.mch, wNNN.mch) */
            if (fname.size() > 4 && fname.substr(fname.size() - 4) == ".mch") {
                std::string stem = fname.substr(0, fname.size() - 4);
                if (low.find("world/") != std::string::npos || stem.rfind("c0w", 0) == 0) {
                    model_choices["world/" + stem] = {int(si), ref, ModelKind::WorldMch, "world", stem};
                    continue;
                }
            }

            /* Pre-built .glb models under field/, battle/, or world/ */
            if (fname.size() > 4 && fname.substr(fname.size() - 4) == ".glb") {
                std::string stem = fname.substr(0, fname.size() - 4);
                std::string cat;
                if (low.find("battle/") != std::string::npos || ps1.battle_models.count(stem)) cat = "battle";
                else if (low.find("world/") != std::string::npos || stem.rfind("c0w", 0) == 0 ||
                         stem == "squall" || stem == "car" || stem == "garden" ||
                         stem == "balamb_garden" || stem == "ragnarok" || stem == "chocobo" || stem == "train") cat = "world";
                else if (low.find("field/") != std::string::npos || ps1.field_models.count(stem)) cat = "field";
                if (!cat.empty() && is_safe_rel_path(stem)) {
                    model_choices[cat + "/" + stem] = {int(si), ref, ModelKind::PrebuiltGlb, cat, stem};
                    continue;
                }
            }

            /* Movies (.avi / .cam / .mp4 / .mkv / .webm) */
            if (!movies_out.empty() && fname.size() > 4) {
                std::string ext4 = fname.substr(fname.size() - 4);
                bool is_mov = (ext4 == ".avi" || ext4 == ".cam" || ext4 == ".mp4" || ext4 == ".mkv" ||
                               (fname.size() > 5 && fname.substr(fname.size() - 5) == ".webm"));
                if (is_mov) {
                    movie_choices[fname] = {int(si), ref, fname};
                    continue;
                }
            }

            /* Audio / music replacement packs (.ogg / .wav / .flac / .mp3) */
            if (!audio_out.empty()) {
                std::string audio_rel;
                if (is_audio_pack_file(low, audio_rel)) {
                    audio_choices[audio_rel] = {int(si), ref, audio_rel};
                    continue;
                }
            }

            /* Textures (.dds / .png) */
            ParsedModTex pt;
            if (parse_ffnx_tex_name(ref.rel_path, pt)) {
                /* Prefer .dds over .png if both exist for same key & palette, or later archive wins. */
                if (pt.pal_idx <= 0) {
                    per_src_tex[si][pt.canonical_key] = {int(si), ref};
                    global_mch_tex[pt.canonical_key] = {int(si), ref};
                }
                if (ps1.textures.count(pt.canonical_key)) {
                    std::string ck = pt.canonical_key + "#" + std::to_string(pt.pal_idx);
                    tex_choices[ck] = {int(si), ref, pt.pal_idx, pt.canonical_key};
                }
            }
        }
    }

    unsigned total_jobs = unsigned(tex_choices.size() + model_choices.size() +
                                   movie_choices.size() + audio_choices.size());
    if (sink) {
        sink->set_total(total_jobs);
        sink->set_stage("Converting HD textures & 3D models...");
    }

    /* Systemic UV-compatibility guard: when a mod replaces a field .mch model with a
     * different TIM count or vertex count than the PS1 disc (e.g. 1-TIM low-poly PS1
     * characters upgraded to 2-TIM high-poly PC meshes), its dNNN-*.dds textures have
     * remapped UVs that ONLY match the .glb model and MUST NOT be written into
     * textures-hd/ (where they would scramble the PS1 low-poly mesh on fallback). */
    std::unordered_set<std::string> remapped_uv_models;
    for (const auto &kv : model_choices) {
        const auto &mc = kv.second;
        if (mc.kind == ModelKind::FieldMch) {
            auto pit = ps1.field_models.find(mc.model_id);
            if (pit == ps1.field_models.end()) continue;
            std::vector<uint8_t> mch;
            if (!read_mod_file(sources[size_t(mc.source_idx)], mc.ref, mch) || mch.size() < 64) continue;
            uint32_t mod_tims = 0;
            size_t pos = 0;
            while (pos + 4 <= mch.size()) {
                uint32_t v = le32(mch.data() + pos);
                pos += 4;
                if (v == 0xFFFFFFFFu) break;
                ++mod_tims;
            }
            if (pos + 4 > mch.size()) continue;
            uint32_t moff = le32(mch.data() + pos);
            if (moff + 64 > mch.size()) continue;
            uint32_t mod_verts = le32(mch.data() + moff + 1 * 4);
            if (mod_tims != pit->second.tim_count || mod_verts != pit->second.vert_count) {
                remapped_uv_models.insert(mc.model_id);
            }
        } else if (mc.kind == ModelKind::BattleDat) {
            auto pit = ps1.battle_models.find(mc.model_id);
            if (pit == ps1.battle_models.end()) continue;
            std::vector<uint8_t> dat;
            if (!read_mod_file(sources[size_t(mc.source_idx)], mc.ref, dat) || dat.size() < 24) continue;
            Ps1ModelInfo mod_info;
            inspect_battle_dat_ps1(dat, mod_info);
            if ((mod_info.tim_count && pit->second.tim_count && mod_info.tim_count != pit->second.tim_count) ||
                (mod_info.vert_count && pit->second.vert_count && mod_info.vert_count != pit->second.vert_count)) {
                remapped_uv_models.insert(mc.model_id);
            }
        }
    }

    /* Step 4: Parallel texture export using a worker thread pool. */
    std::vector<TexChoice> tex_jobs;
    tex_jobs.reserve(tex_choices.size());
    for (auto &kv : tex_choices) {
        size_t dash = kv.second.key.find('-');
        std::string base = (dash == std::string::npos) ? kv.second.key : kv.second.key.substr(0, dash);
        if (remapped_uv_models.count(base)) continue;
        tex_jobs.push_back(std::move(kv.second));
    }

    std::atomic<size_t> next_tex{0};
    std::atomic<unsigned> tex_written{0};
    std::atomic<unsigned> tex_skipped{0};
    std::atomic<unsigned> tex_failed{0};
    std::mutex written_mu;
    std::vector<std::string> written_tex_files;
    std::vector<std::string> written_model_files;
    json written_manifest_models = json::object();
    std::vector<std::string> written_movie_files;
    std::vector<std::string> written_audio_files;
    if (!remapped_uv_models.empty()) {
        add_log("UV guard kept " + std::to_string(remapped_uv_models.size()) +
                " remapped model texture sets inside .glb only");
    }

    if (!textures_out.empty() && !tex_jobs.empty()) {
        std::error_code ec;
        fs::create_directories(textures_out, ec);
        unsigned nthreads = std::max(1u, std::min(8u, std::thread::hardware_concurrency()));
        nthreads = std::min<unsigned>(nthreads, unsigned(tex_jobs.size()));
        std::vector<std::thread> workers;
        workers.reserve(nthreads);

        for (unsigned t = 0; t < nthreads; ++t) {
            workers.emplace_back([&]() {
                while (true) {
                    size_t idx = next_tex.fetch_add(1);
                    if (idx >= tex_jobs.size()) break;
                    const auto &job = tex_jobs[idx];
                    auto pit = ps1.textures.find(job.key);
                    if (pit == ps1.textures.end()) {
                        if (sink) sink->inc_done();
                        continue;
                    }
                    std::vector<uint8_t> raw;
                    if (!read_mod_file(sources[size_t(job.source_idx)], job.ref, raw)) {
                        ++tex_failed;
                        if (sink) sink->inc_done();
                        continue;
                    }
                    DdsImage decoded;
                    if (!decode_image_bytes(raw.data(), raw.size(), decoded)) {
                        ++tex_failed;
                        if (sink) sink->inc_done();
                        continue;
                    }

                    for (const auto &target : pit->second) {
                        DdsImage work = decoded;

                        /* Crop right-padded FFNx battle stage textures (e.g. 3072x1024 -> 2696x1024
                         * for PS1 674x256) so normalized UVs align 1:1 with zero stretch. */
                        if (target.is_battle_stage && target.target_h > 0 && target.target_w > 0 &&
                            work.height >= target.target_h && (work.height % target.target_h) == 0) {
                            uint32_t scale = work.height / target.target_h;
                            if (scale > 0 && (scale & (scale - 1u)) == 0u &&
                                work.width > target.target_w * scale) {
                                work = crop_rgba(work, 0, 0, target.target_w * scale, work.height);
                            }
                        }

                        /* Subregion crop for FF8 field eye-blink split sheets (rows 112/114/116). */
                        if (target.is_subregion && target.full_h > 0) {
                            uint32_t sy0 = uint32_t(uint64_t(target.sub_y0) * work.height / target.full_h);
                            uint32_t sy1 = uint32_t(uint64_t(target.sub_y1) * work.height / target.full_h);
                            if (sy1 > sy0 && sy1 <= work.height) {
                                work = crop_rgba(work, 0, sy0, work.width, sy1 - sy0);
                            }
                        }

                        /* Ensure dimensions are a clean multiple of target_w/target_h if slightly off. */
                        if (target.target_w > 0 && target.target_h > 0 &&
                            work.width >= target.target_w && work.height >= target.target_h) {
                            uint32_t sx = std::max(1u, (work.width + target.target_w / 2u) / target.target_w);
                            uint32_t sy = std::max(1u, (work.height + target.target_h / 2u) / target.target_h);
                            uint32_t s = std::max(sx, sy);
                            /* Snap to power-of-two scale up to 16x for parallel-psx HdTextureProvider. */
                            uint32_t pot = 1;
                            while (pot < s && pot < 16u) pot <<= 1;
                            if (pot > 1u && std::abs(int(work.width) - int(target.target_w * pot)) <= int(pot) &&
                                std::abs(int(work.height) - int(target.target_h * pot)) <= int(pot) &&
                                (work.width != target.target_w * pot || work.height != target.target_h * pot)) {
                                work = resize_rgba(work, target.target_w * pot, target.target_h * pot);
                            }
                        }

                        std::vector<uint8_t> png;
                        if (!encode_rgba_png(work.width, work.height, work.rgba.data(), png)) {
                            ++tex_failed;
                            continue;
                        }

                        std::string htex = hex64(target.tex_hash);
                        if (job.pal_idx <= 0) {
                            std::string fname_out = htex + ".png";
                            fs::path out_p = fs::path(textures_out) / fname_out;
                            std::unique_lock<std::mutex> lk(written_mu, std::defer_lock);
                            if (managed) lk.lock();
                            if (!occupied_output(out_p)) {
                                if (write_file_bytes(out_p, png.data(), png.size())) {
                                    ++tex_written;
                                    if (!lk.owns_lock()) lk.lock();
                                    written_tex_files.push_back(std::move(fname_out));
                                } else ++tex_failed;
                            }
                        }
                        if (job.pal_idx >= 0 && size_t(job.pal_idx) < target.clut_hashes.size()) {
                            std::string hclut = hex64(target.clut_hashes[size_t(job.pal_idx)]);
                            std::string fname_out = htex + "_" + hclut + ".png";
                            fs::path out_p = fs::path(textures_out) / fname_out;
                            std::unique_lock<std::mutex> lk(written_mu, std::defer_lock);
                            if (managed) lk.lock();
                            if (!occupied_output(out_p)) {
                                if (write_file_bytes(out_p, png.data(), png.size())) {
                                    ++tex_written;
                                    if (!lk.owns_lock()) lk.lock();
                                    written_tex_files.push_back(std::move(fname_out));
                                } else ++tex_failed;
                            }
                        }
                    }
                    if (sink) sink->inc_done();
                }
            });
        }
        for (auto &w : workers) w.join();
    }

    /* Step 5: Export high-poly field, battle, and world 3D models (.mch/.dat/.glb + manifest.json). */
    unsigned models_written = 0;
    unsigned models_failed = 0;
    if (!models_out.empty() && !model_choices.empty()) {
        if (sink) sink->set_stage("Converting HD 3D models...");
        std::error_code ec;
        fs::create_directories(fs::path(models_out) / "field", ec);
        fs::create_directories(fs::path(models_out) / "battle", ec);
        fs::create_directories(fs::path(models_out) / "world", ec);

        json manifest = {{"models", json::object()}};
        /* Preserve any pre-existing custom models in manifest.json */
        fs::path manifest_path = fs::path(models_out) / "manifest.json";
        if (fs::is_regular_file(manifest_path, ec)) {
            std::ifstream min(manifest_path);
            try {
                json existing = json::parse(min);
                if (existing.is_object() && existing.contains("models") && existing["models"].is_object()) {
                    manifest = std::move(existing);
                }
            } catch (...) {}
        }

        /* Sort canonical keys ascending so primary models own shared VRAM hashes. */
        auto ps1_info_for = [&](const ModelChoice &mc) -> const Ps1ModelInfo * {
            if (mc.category == "field") {
                auto it = ps1.field_models.find(mc.model_id);
                return it != ps1.field_models.end() ? &it->second : nullptr;
            }
            if (mc.category == "battle") {
                auto it = ps1.battle_models.find(mc.model_id);
                return it != ps1.battle_models.end() ? &it->second : nullptr;
            }
            auto it = ps1.world_models.find(mc.model_id);
            return it != ps1.world_models.end() ? &it->second : nullptr;
        };

        std::vector<std::string> sorted_keys;
        sorted_keys.reserve(model_choices.size());
        std::unordered_map<uint64_t, unsigned> hash_owner_count;
        for (const auto &kv : model_choices) {
            sorted_keys.push_back(kv.first);
            if (const Ps1ModelInfo *pi = ps1_info_for(kv.second)) {
                for (uint64_t h : pi->vram_texture_hashes) ++hash_owner_count[h];
            }
        }
        std::sort(sorted_keys.begin(), sorted_keys.end());
        std::unordered_set<uint64_t> claimed_hashes;

        for (const auto &canon_key : sorted_keys) {
            const auto &mc = model_choices.at(canon_key);
            const Ps1ModelInfo *ps1_ptr = ps1_info_for(mc);
            Ps1ModelInfo empty_ps1;
            const Ps1ModelInfo &ps1_ref = ps1_ptr ? *ps1_ptr : empty_ps1;

            std::vector<uint8_t> raw_bytes;
            if (!read_mod_file(sources[size_t(mc.source_idx)], mc.ref, raw_bytes)) {
                ++models_failed;
                if (sink) sink->inc_done();
                continue;
            }

            MchTexProvider prov;
            prov.same_src = &per_src_tex[size_t(mc.source_idx)];
            prov.global = &global_mch_tex;
            prov.sources = &sources;

            fs::path rel_glb = fs::path(mc.category) / (mc.model_id + ".glb");
            fs::path abs_glb = fs::path(models_out) / rel_glb;
            const bool model_key_exists = managed && manifest["models"].contains(canon_key);
            if (model_key_exists) ++conflicts;
            if (model_key_exists || occupied_output(abs_glb)) {
                if (sink) sink->inc_done();
                continue;
            }
            std::vector<int> bone_remap;
            bool ok = false;
            if (mc.kind == ModelKind::FieldMch || mc.kind == ModelKind::WorldMch) {
                ok = convert_mch_to_glb(mc.model_id, raw_bytes, ps1_ref, prov, abs_glb, bone_remap);
            } else if (mc.kind == ModelKind::BattleDat) {
                ok = convert_battle_dat_to_glb(mc.model_id, raw_bytes, ps1_ref, prov, abs_glb, bone_remap);
            } else if (mc.kind == ModelKind::PrebuiltGlb) {
                ok = raw_bytes.size() >= 12 && std::memcmp(raw_bytes.data(), "glTF", 4) == 0 &&
                     write_file_bytes(abs_glb, raw_bytes.data(), raw_bytes.size());
            }

            if (ok) {
                json entry = {
                    {"file", rel_glb.generic_string()},
                    {"scale", 1.0},
                    {"double_sided", true}
                };
                if (!bone_remap.empty()) entry["bone_remap"] = bone_remap;
                json hashes = json::array();
                for (uint64_t h : ps1_ref.vram_texture_hashes) {
                    if (hash_owner_count[h] == 1u) {
                        hashes.push_back(hex64(h));
                        claimed_hashes.insert(h);
                    }
                }
                if (hashes.empty()) {
                    for (uint64_t h : ps1_ref.vram_texture_hashes) {
                        if (claimed_hashes.insert(h).second) hashes.push_back(hex64(h));
                    }
                }
                if (!hashes.empty()) entry["vram_texture_hashes"] = hashes;
                written_manifest_models[canon_key] = entry;
                written_model_files.push_back(rel_glb.generic_string());
                manifest["models"][canon_key] = std::move(entry);
                ++models_written;
            } else {
                ++models_failed;
            }
            if (sink) sink->inc_done();
        }

        std::string mstr = manifest.dump(2) + "\n";
        if (models_written || !managed)
            write_file_bytes(manifest_path, reinterpret_cast<const uint8_t *>(mstr.data()), mstr.size());
    }

    /* Step 6: Optional FMV movie extraction (.avi / .cam / .mp4 / .mkv / .webm). */
    unsigned movies_written = 0;
    if (!movies_out.empty() && !movie_choices.empty()) {
        if (sink) sink->set_stage("Extracting HD movies...");
        std::error_code ec;
        fs::create_directories(movies_out, ec);
        for (const auto &kv : movie_choices) {
            const auto &mc = kv.second;
            fs::path target = fs::path(movies_out) / mc.filename;
            if (occupied_output(target)) {
                if (sink) sink->inc_done();
                continue;
            }
            if (fs::is_regular_file(target, ec) && fs::file_size(target, ec) == mc.ref.size) {
                ++movies_written;
                written_movie_files.push_back(mc.filename);
                if (sink) sink->inc_done();
                continue;
            }
            std::vector<uint8_t> data;
            if (read_mod_file(sources[size_t(mc.source_idx)], mc.ref, data) &&
                write_file_bytes(target, data.data(), data.size())) {
                ++movies_written;
                written_movie_files.push_back(mc.filename);
            }
            if (sink) sink->inc_done();
        }
    }

    /* Step 7: Optional audio / music pack extraction (.ogg / .wav / .flac / .mp3). */
    unsigned audio_written = 0;
    if (!audio_out.empty() && !audio_choices.empty()) {
        if (sink) sink->set_stage("Extracting audio pack...");
        std::error_code ec;
        fs::create_directories(audio_out, ec);
        for (const auto &kv : audio_choices) {
            const auto &ac = kv.second;
            fs::path target = fs::path(audio_out) / ac.rel_path;
            fs::create_directories(target.parent_path(), ec);
            if (occupied_output(target)) {
                if (sink) sink->inc_done();
                continue;
            }
            if (fs::is_regular_file(target, ec) && fs::file_size(target, ec) == ac.ref.size) {
                ++audio_written;
                written_audio_files.push_back(ac.rel_path);
                if (sink) sink->inc_done();
                continue;
            }
            std::vector<uint8_t> data;
            if (read_mod_file(sources[size_t(ac.source_idx)], ac.ref, data) &&
                write_file_bytes(target, data.data(), data.size())) {
                ++audio_written;
                written_audio_files.push_back(ac.rel_path);
            }
            if (sink) sink->inc_done();
        }
        if (audio_written > 0) {
            add_log("Extracted " + std::to_string(audio_written) +
                    " audio/music files to audio-hd/ (external audio playback pack staged)");
        }
    }

    if (tex_failed.load() > 0) {
        add_log(std::to_string(tex_failed.load()) + " texture(s) failed to decode or write", true);
    }
    if (models_failed > 0) {
        add_log(std::to_string(models_failed) + " 3D model(s) failed to convert", true);
    }
    if (!overrides.empty()) {
        std::string ov = "Applied " + std::to_string(overrides.size()) + " custom mod.xml option(s)";
        add_log(ov);
    }

    if (conflicts) add_log("Preserved " + std::to_string(conflicts.load()) + " existing asset file(s)", true);

    /* Step 8: Persist mod entry in <registry_dir>/mods-registry.json if requested. */
    if (managed) {
        auto dedup = [](std::vector<std::string> &v) {
            std::sort(v.begin(), v.end());
            v.erase(std::unique(v.begin(), v.end()), v.end());
        };
        dedup(written_tex_files);
        dedup(written_model_files);
        dedup(written_movie_files);
        dedup(written_audio_files);

        std::string mod_name = !primary_mod_name.empty() ? primary_mod_name
                             : (!sources.empty() ? sources.front().label : fs::path(mod_paths.front()).filename().string());
        const std::string slug = slugify_mod_id(mod_name);
        std::string mod_id = slug;
        const auto &mods = registry["mods"];
        for (unsigned suffix = 2; std::any_of(mods.begin(), mods.end(), [&](const json &entry) {
                 return entry.is_object() && entry.value("id", "") == mod_id;
             }); ++suffix) {
            mod_id = slug + "-" + std::to_string(suffix);
        }

        json rec = {
            {"id", mod_id},
            {"name", mod_name},
            {"source", mod_paths.front()},
            {"enabled", true},
            {"textures_written", tex_written.load()},
            {"models_written", models_written},
            {"movies_written", movies_written},
            {"audio_written", audio_written},
            {"warnings", warnings},
            {"failed", tex_failed.load() + models_failed},
            {"log", log_lines},
            {"roots", roots},
            {"files", {
                {"textures", written_tex_files},
                {"models", written_model_files},
                {"movies", written_movie_files},
                {"audio", written_audio_files}
            }},
            {"manifest_models", written_manifest_models}
        };

        registry["mods"].push_back(std::move(rec));
        if (!save_registry_json(reg_path, registry)) {
            std::snprintf(final_status.error, sizeof(final_status.error), "Could not save the mod registry.");
            final_status.running = 0;
            final_status.finished = 1;
            return false;
        }
    }

    final_status.running = 0;
    final_status.finished = 1;
    final_status.ok = 1;
    final_status.done = total_jobs;
    final_status.total = total_jobs;
    final_status.textures_written = tex_written.load();
    final_status.textures_skipped = tex_skipped.load();
    final_status.models_written = models_written;
    final_status.movies_written = movies_written;
    final_status.audio_written = audio_written;
    final_status.warnings = warnings;
    final_status.failed = tex_failed.load() + models_failed;
    std::snprintf(final_status.stage, sizeof(final_status.stage), "Done");
    return true;
}

/* ---------- Async GUI Worker State ---------- */

struct AsyncState {
    std::mutex mu;
    std::thread worker;
    ProgressSink sink;
    bool active = false;
};

AsyncState &async_state() {
    static AsyncState s;
    return s;
}

} // namespace

bool evaluate_active_when_expr(const std::string &expr,
                               const std::vector<std::pair<std::string, int>> &vars) {
    std::unordered_map<std::string, int> map;
    for (const auto &kv : vars) map[to_lower(kv.first)] = kv.second;
    ExprParser p(expr, map);
    return p.eval();
}

bool parse_mod_xml_config(const std::string &xml, host_mod_xml_config_t &out) {
    out = {};
    if (xml.empty()) return false;
    std::string low = to_lower(xml);

    /* Extract top-level <Name>, <Author>, <Version> before the first <ConfigOption> or <ModFolder>. */
    size_t hdr_end = xml.size();
    size_t p_cfg = low.find("<configoption");
    size_t p_fld = low.find("<modfolder");
    if (p_cfg != std::string::npos) hdr_end = std::min(hdr_end, p_cfg);
    if (p_fld != std::string::npos) hdr_end = std::min(hdr_end, p_fld);
    std::string hdr = xml.substr(0, hdr_end);
    std::string lhdr = low.substr(0, hdr_end);
    auto extract_hdr = [&](const char *tag) -> std::string {
        std::string open = std::string("<") + tag + ">";
        std::string close = std::string("</") + tag + ">";
        size_t a = lhdr.find(open);
        if (a == std::string::npos) return "";
        size_t b = lhdr.find(close, a + open.size());
        if (b == std::string::npos) return "";
        return trim(xml_unescape(hdr.substr(a + open.size(), b - (a + open.size()))));
    };
    std::snprintf(out.mod_name, sizeof(out.mod_name), "%s", extract_hdr("name").c_str());
    std::snprintf(out.author, sizeof(out.author), "%s", extract_hdr("author").c_str());
    std::snprintf(out.version, sizeof(out.version), "%s", extract_hdr("version").c_str());

    size_t pos = 0;
    while (out.option_count < HOST_MOD_MAX_OPTIONS &&
           (pos = low.find("<configoption", pos)) != std::string::npos) {
        size_t end = low.find("</configoption>", pos);
        if (end == std::string::npos) break;
        std::string block = xml.substr(pos, end - pos);
        std::string lblock = low.substr(pos, end - pos);
        pos = end + 15;

        auto extract_tag_range = [](const std::string &blk, const std::string &lblk, const char *name) -> std::string {
            std::string open = std::string("<") + name + ">";
            std::string close = std::string("</") + name + ">";
            size_t a = lblk.find(open);
            if (a == std::string::npos) return "";
            size_t b = lblk.find(close, a + open.size());
            if (b == std::string::npos) return "";
            return trim(xml_unescape(blk.substr(a + open.size(), b - (a + open.size()))));
        };

        /* Stop header extraction of <Name> before the first <Option so an option's <Name> isn't stolen. */
        size_t first_opt = lblock.find("<option");
        std::string opt_hdr = first_opt == std::string::npos ? block : block.substr(0, first_opt);
        std::string lopt_hdr = first_opt == std::string::npos ? lblock : lblock.substr(0, first_opt);

        std::string id = extract_tag_range(opt_hdr, lopt_hdr, "id");
        if (id.empty()) continue;
        std::string name = extract_tag_range(opt_hdr, lopt_hdr, "name");
        if (name.empty()) name = id;
        std::string desc = extract_tag_range(opt_hdr, lopt_hdr, "description");
        std::string def = extract_tag_range(opt_hdr, lopt_hdr, "default");
        std::string type_s = to_lower(extract_tag_range(opt_hdr, lopt_hdr, "type"));

        host_mod_xml_option_t &opt = out.options[out.option_count];
        opt = {};
        std::snprintf(opt.id, sizeof(opt.id), "%s", id.c_str());
        std::snprintf(opt.name, sizeof(opt.name), "%s", name.c_str());
        std::snprintf(opt.description, sizeof(opt.description), "%s", desc.c_str());
        opt.default_value = def.empty() ? 0 : std::atoi(def.c_str());
        opt.selected_value = opt.default_value;
        opt.type = (type_s == "bool" || type_s == "boolean") ? 0 : 1;

        /* Parse <Option Value="..." Name="..." /> or <Option><Value>..</Value><Name>..</Name></Option> */
        size_t opos = 0;
        while (opt.choice_count < HOST_MOD_MAX_CHOICES &&
               (opos = lblock.find("<option", opos)) != std::string::npos) {
            size_t gt = block.find('>', opos);
            if (gt == std::string::npos) break;
            std::string tag = block.substr(opos, gt - opos + 1);
            bool self_close = (gt > opos && block[gt - 1] == '/');
            std::string val_s, cname;
            extract_xml_attr(tag, "value", val_s);
            extract_xml_attr(tag, "name", cname);
            size_t next_o = gt + 1;
            if (!self_close) {
                size_t ocl = lblock.find("</option>", gt + 1);
                if (ocl != std::string::npos) {
                    std::string obody = block.substr(gt + 1, ocl - (gt + 1));
                    std::string lobody = lblock.substr(gt + 1, ocl - (gt + 1));
                    if (val_s.empty()) val_s = extract_tag_range(obody, lobody, "value");
                    if (cname.empty()) cname = extract_tag_range(obody, lobody, "name");
                    next_o = ocl + 9;
                }
            }
            opos = next_o;
            if (val_s.empty() && cname.empty()) continue;
            host_mod_xml_choice_t &ch = opt.choices[opt.choice_count++];
            ch.value = std::atoi(val_s.c_str());
            std::snprintf(ch.name, sizeof(ch.name), "%s", cname.empty() ? val_s.c_str() : cname.c_str());
        }

        if (opt.choice_count == 0) {
            opt.type = 0;
            opt.choice_count = 2;
            opt.choices[0].value = 0; std::snprintf(opt.choices[0].name, sizeof(opt.choices[0].name), "Off");
            opt.choices[1].value = 1; std::snprintf(opt.choices[1].name, sizeof(opt.choices[1].name), "On");
        } else if (opt.choice_count == 2 && opt.choices[0].value == 0 && opt.choices[1].value == 1 &&
                   (type_s == "bool" || type_s == "boolean")) {
            opt.type = 0;
        } else {
            opt.type = 1;
        }
        ++out.option_count;
    }

    return out.mod_name[0] != 0 || out.option_count > 0;
}

ModXmlResult parse_and_eval_mod_xml(const std::string &xml,
                                    const std::vector<std::pair<std::string, int>> &overrides) {
    ModXmlResult out;
    std::string low = to_lower(xml);

    /* 1. Extract <ConfigOption> <ID> + <Default> pairs, then apply user overrides. */
    std::unordered_map<std::string, int> defaults;
    size_t pos = 0;
    while ((pos = low.find("<configoption", pos)) != std::string::npos) {
        size_t end = low.find("</configoption>", pos);
        if (end == std::string::npos) break;
        std::string block = xml.substr(pos, end - pos);
        std::string lblock = low.substr(pos, end - pos);

        auto extract_tag = [&](const char *name) -> std::string {
            std::string open = std::string("<") + name + ">";
            std::string close = std::string("</") + name + ">";
            size_t a = lblock.find(open);
            if (a == std::string::npos) return "";
            size_t b = lblock.find(close, a + open.size());
            if (b == std::string::npos) return "";
            return trim(xml_unescape(block.substr(a + open.size(), b - (a + open.size()))));
        };

        std::string id = to_lower(extract_tag("id"));
        std::string def = extract_tag("default");
        if (!id.empty() && !def.empty()) {
            defaults[id] = std::atoi(def.c_str());
        }
        pos = end + 15;
    }
    for (const auto &ov : overrides) {
        if (!ov.first.empty()) defaults[to_lower(ov.first)] = ov.second;
    }

    /* 2. Extract <ModFolder Folder="..." [ActiveWhen="..."]> [ <ActiveWhen>...</ActiveWhen> ] */
    pos = 0;
    while ((pos = low.find("<modfolder", pos)) != std::string::npos) {
        size_t gt = xml.find('>', pos);
        if (gt == std::string::npos) break;
        std::string open_tag = xml.substr(pos, gt - pos + 1);
        bool self_closing = (gt > pos && xml[gt - 1] == '/');

        std::string folder;
        std::string active_attr;
        extract_xml_attr(open_tag, "folder", folder);
        bool has_attr = extract_xml_attr(open_tag, "activewhen", active_attr);

        std::string body;
        size_t next_pos = gt + 1;
        if (!self_closing) {
            size_t cl = low.find("</modfolder>", gt + 1);
            if (cl != std::string::npos) {
                body = xml.substr(gt + 1, cl - (gt + 1));
                next_pos = cl + 12;
            }
        }
        pos = next_pos;

        std::string norm = to_lower(normalize_slashes(trim(folder)));
        if (norm.empty() || norm == "no change") continue;

        out.has_folder_rules = true;
        out.declared_folders.push_back(norm);

        bool is_active = false;
        if (has_attr && !trim(active_attr).empty()) {
            ExprParser ep(active_attr, defaults);
            is_active = ep.eval();
        } else if (!body.empty()) {
            std::string lbody = to_lower(body);
            size_t a0 = lbody.find("<activewhen");
            if (a0 != std::string::npos) {
                size_t agt = body.find('>', a0);
                size_t a1 = lbody.find("</activewhen>", agt == std::string::npos ? a0 : agt + 1);
                if (agt != std::string::npos && a1 != std::string::npos) {
                    is_active = eval_active_when_block(body.substr(agt + 1, a1 - (agt + 1)), defaults);
                }
            } else {
                /* Unconditional <ModFolder Folder="..."> */
                is_active = true;
            }
        } else {
            /* Unconditional <ModFolder Folder="..." /> */
            is_active = true;
        }

        if (is_active) out.active_folders.push_back(norm);
    }

    return out;
}

bool parse_iros_directory(const std::string &path, std::vector<IrosEntry> &out, std::string *error) {
    out.clear();
    std::error_code ec;
    uint64_t fsize = fs::file_size(path, ec);
    if (ec || fsize < 16) {
        if (error) *error = "truncated IROS header";
        return false;
    }
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        if (error) *error = "cannot open IROS archive";
        return false;
    }
    uint8_t hdr[16];
    in.read(reinterpret_cast<char *>(hdr), 16);
    if (in.gcount() != 16 || std::memcmp(hdr, "IROS", 4) != 0) {
        if (error) *error = "not an IROS archive";
        return false;
    }
    uint32_t version = le32(hdr + 4);
    uint32_t flags = le32(hdr + 8);
    uint32_t dir_off = le32(hdr + 12);
    if (version != 0x10002u || flags != 0u || dir_off < 16u || uint64_t(dir_off) > fsize - 4u) {
        if (error) *error = "unsupported IROS header";
        return false;
    }

    in.seekg(std::streamoff(dir_off));
    uint8_t cnt_raw[4];
    in.read(reinterpret_cast<char *>(cnt_raw), 4);
    if (in.gcount() != 4) return false;
    uint32_t count = le32(cnt_raw);
    if (count > 200000u) {
        if (error) *error = "invalid IROS entry count";
        return false;
    }

    out.reserve(count);
    std::vector<uint8_t> name_buf;
    for (uint32_t i = 0; i < count; ++i) {
        uint64_t rec_start = uint64_t(in.tellg());
        uint8_t rh[4];
        in.read(reinterpret_cast<char *>(rh), 4);
        if (in.gcount() != 4) return false;
        uint16_t rec_len = le16(rh + 0);
        uint16_t name_len = le16(rh + 2);
        if ((name_len & 1u) != 0u || rec_len < name_len + 20u || rec_start + rec_len > fsize) {
            if (error) *error = "invalid IROS directory record";
            return false;
        }
        name_buf.resize(name_len);
        in.read(reinterpret_cast<char *>(name_buf.data()), name_len);
        if (uint16_t(in.gcount()) != name_len) return false;

        uint8_t meta[16];
        in.read(reinterpret_cast<char *>(meta), 16);
        if (in.gcount() != 16) return false;

        std::string name = utf16le_to_utf8(name_buf.data(), name_len);
        std::replace(name.begin(), name.end(), '\\', '/');
        if (name.empty() || name[0] == '/' || name.find("..") != std::string::npos) {
            in.seekg(std::streamoff(rec_start + rec_len));
            continue;
        }
        IrosEntry e;
        e.name = std::move(name);
        e.flags = le32(meta + 0);
        e.offset = le64(meta + 4);
        e.size = le32(meta + 12);
        if (e.offset <= fsize && e.size <= fsize - e.offset) {
            out.push_back(std::move(e));
        }
        in.seekg(std::streamoff(rec_start + rec_len));
    }
    return true;
}

bool convert_battle_dat_to_glb_file(const std::string &model_id,
                                    const std::vector<uint8_t> &dat_bytes,
                                    const std::string &glb_out_path,
                                    std::vector<int> *out_bone_remap) {
    Ps1ModelInfo ps1_info;
    inspect_battle_dat_ps1(dat_bytes, ps1_info);
    MchTexProvider empty_prov;
    std::vector<int> remap;
    bool ok = convert_battle_dat_to_glb(model_id, dat_bytes, ps1_info, empty_prov, fs::path(glb_out_path), remap);
    if (ok && out_bone_remap) *out_bone_remap = std::move(remap);
    return ok;
}

} // namespace HostHD

extern "C" int host_mod_inspect_options(const char *mod_path, host_mod_xml_config_t *out_config) {
    if (!mod_path || !mod_path[0] || !out_config) return 0;
    *out_config = {};
    std::string xml, label;
    if (!HostHD::read_mod_xml_from_path(mod_path, xml, label)) return 0;
    bool ok = HostHD::parse_mod_xml_config(xml, *out_config);
    if (ok && !out_config->mod_name[0] && !label.empty()) {
        std::snprintf(out_config->mod_name, sizeof(out_config->mod_name), "%s", label.c_str());
    }
    return ok ? 1 : 0;
}

extern "C" int host_mod_import_run_ex(const char *const *disc_paths, unsigned disc_count,
                                      const char *const *mod_paths, unsigned mod_count,
                                      const char *textures_out, const char *models_out,
                                      const char *movies_out, const char *audio_out,
                                      const char *registry_dir,
                                      const host_mod_option_override_t *overrides, unsigned override_count,
                                      host_mod_import_status_t *out_status) {
    std::vector<std::string> discs, mods;
    for (unsigned i = 0; i < disc_count; ++i) {
        if (disc_paths && disc_paths[i] && disc_paths[i][0]) discs.emplace_back(disc_paths[i]);
    }
    for (unsigned i = 0; i < mod_count; ++i) {
        if (mod_paths && mod_paths[i] && mod_paths[i][0]) mods.emplace_back(mod_paths[i]);
    }
    std::vector<std::pair<std::string, int>> ovs;
    for (unsigned i = 0; i < override_count; ++i) {
        if (overrides && overrides[i].id[0]) ovs.emplace_back(overrides[i].id, overrides[i].value);
    }
    host_mod_import_status_t st{};
    bool ok = HostHD::run_import_impl(discs, mods,
                                      textures_out ? textures_out : "",
                                      models_out ? models_out : "",
                                      movies_out ? movies_out : "",
                                      audio_out ? audio_out : "",
                                      registry_dir ? registry_dir : "",
                                      ovs, nullptr, st);
    if (out_status) *out_status = st;
    return ok ? 1 : 0;
}

extern "C" int host_mod_import_run(const char *const *disc_paths, unsigned disc_count,
                                   const char *const *mod_paths, unsigned mod_count,
                                   const char *textures_out, const char *models_out,
                                   const char *movies_out,
                                   host_mod_import_status_t *out_status) {
    return host_mod_import_run_ex(disc_paths, disc_count, mod_paths, mod_count,
                                  textures_out, models_out, movies_out, nullptr, nullptr,
                                  nullptr, 0, out_status);
}

extern "C" int host_mod_import_start_async_ex(const char *const *disc_paths, unsigned disc_count,
                                              const char *mod_path,
                                              const char *textures_out, const char *models_out,
                                              const char *movies_out, const char *audio_out,
                                              const char *registry_dir,
                                              const host_mod_option_override_t *overrides, unsigned override_count) {
    if (!mod_path || !mod_path[0]) return 0;
    auto &as = HostHD::async_state();
    std::lock_guard<std::mutex> lk(as.mu);
    if (as.active) return 0;
    if (as.worker.joinable()) as.worker.join();

    std::vector<std::string> discs;
    for (unsigned i = 0; i < disc_count; ++i) {
        if (disc_paths && disc_paths[i] && disc_paths[i][0]) discs.emplace_back(disc_paths[i]);
    }
    std::vector<std::string> mods = {mod_path};
    std::string tex = textures_out ? textures_out : "";
    std::string mod = models_out ? models_out : "";
    std::string mov = movies_out ? movies_out : "";
    std::string aud = audio_out ? audio_out : "";
    std::string reg = registry_dir ? registry_dir : "";
    std::vector<std::pair<std::string, int>> ovs;
    for (unsigned i = 0; i < override_count; ++i) {
        if (overrides && overrides[i].id[0]) ovs.emplace_back(overrides[i].id, overrides[i].value);
    }

    {
        std::lock_guard<std::mutex> slk(as.sink.mu);
        as.sink.st = {};
        as.sink.st.running = 1;
        std::snprintf(as.sink.st.stage, sizeof(as.sink.st.stage), "Starting mod import...");
    }
    as.active = true;
    as.worker = std::thread([&as, discs, mods, tex, mod, mov, aud, reg, ovs]() {
        host_mod_import_status_t done_st{};
        HostHD::run_import_impl(discs, mods, tex, mod, mov, aud, reg, ovs, &as.sink, done_st);
        {
            std::lock_guard<std::mutex> slk(as.sink.mu);
            as.sink.st = done_st;
        }
        std::lock_guard<std::mutex> lk2(as.mu);
        as.active = false;
    });
    return 1;
}

extern "C" int host_mod_import_start_async(const char *const *disc_paths, unsigned disc_count,
                                           const char *mod_path,
                                           const char *textures_out, const char *models_out,
                                           const char *movies_out) {
    return host_mod_import_start_async_ex(disc_paths, disc_count, mod_path,
                                          textures_out, models_out, movies_out,
                                          nullptr, nullptr, nullptr, 0);
}

extern "C" void host_mod_import_poll(host_mod_import_status_t *out_status) {
    if (!out_status) return;
    auto &as = HostHD::async_state();
    std::lock_guard<std::mutex> slk(as.sink.mu);
    *out_status = as.sink.st;
}

extern "C" void host_mod_import_shutdown(void) {
    auto &as = HostHD::async_state();
    std::thread to_join;
    {
        std::lock_guard<std::mutex> lk(as.mu);
        if (as.worker.joinable()) to_join = std::move(as.worker);
    }
    if (to_join.joinable()) to_join.join();
}

extern "C" unsigned host_mod_registry_list(const char *data_dir, host_mod_entry_t *out, unsigned capacity) {
    if (!data_dir || !data_dir[0]) return 0;
    nlohmann::json doc;
    if (!HostHD::load_registry_json(std::filesystem::path(data_dir) / "mods-registry.json", doc)) return 0;
    unsigned count = 0;
    for (const auto &m : doc["mods"]) {
        if (!m.is_object()) continue;
        if (out && count < capacity) {
            host_mod_entry_t &e = out[count];
            e = {};
            std::snprintf(e.id, sizeof(e.id), "%s", m.value("id", "").c_str());
            std::snprintf(e.name, sizeof(e.name), "%s", m.value("name", "").c_str());
            std::snprintf(e.source, sizeof(e.source), "%s", m.value("source", "").c_str());
            e.enabled = m.value("enabled", true) ? 1 : 0;
            e.textures_written = m.value("textures_written", 0u);
            e.models_written = m.value("models_written", 0u);
            e.movies_written = m.value("movies_written", 0u);
            e.audio_written = m.value("audio_written", 0u);
            e.warnings = m.value("warnings", 0u);
            e.failed = m.value("failed", 0u);
            std::string log_joined;
            if (m.contains("log") && m["log"].is_array()) {
                for (const auto &line : m["log"]) {
                    if (!line.is_string()) continue;
                    if (!log_joined.empty()) log_joined.push_back('\n');
                    log_joined += line.get<std::string>();
                }
            }
            std::snprintf(e.summary_log, sizeof(e.summary_log), "%s", log_joined.c_str());
        }
        ++count;
    }
    return count;
}

namespace {
bool registry_matches_root(const nlohmann::json &entry, const char *bucket, const std::string &dir) {
    if (!entry.contains("roots")) return true;
    const auto &roots = entry["roots"];
    return roots.is_object() && roots.contains(bucket) && roots[bucket].is_string() &&
           roots[bucket] == HostHD::asset_root(dir);
}

void rebuild_models_manifest_from_registry(const nlohmann::json &doc, const std::string &models_dir) {
    if (models_dir.empty()) return;
    namespace fs = std::filesystem;
    fs::path manifest_path = fs::path(models_dir) / "manifest.json";
    nlohmann::json manifest = {{"models", nlohmann::json::object()}};
    std::error_code ec;
    if (fs::is_regular_file(manifest_path, ec)) {
        std::ifstream min(manifest_path);
        try {
            nlohmann::json existing = nlohmann::json::parse(min);
            if (existing.is_object() && existing.contains("models") && existing["models"].is_object()) {
                manifest = std::move(existing);
            }
        } catch (...) {}
    }
    /* Remove keys owned by disabled mods, then reapply enabled mods. */
    for (const auto &m : doc["mods"]) {
        if (!m.is_object() || !registry_matches_root(m, "models", models_dir) ||
            !m.contains("manifest_models") || !m["manifest_models"].is_object()) continue;
        bool en = m.value("enabled", true);
        for (auto it = m["manifest_models"].begin(); it != m["manifest_models"].end(); ++it) {
            if (!en) manifest["models"].erase(it.key());
        }
    }
    for (const auto &m : doc["mods"]) {
        if (!m.is_object() || !m.value("enabled", true) || !registry_matches_root(m, "models", models_dir)) continue;
        if (!m.contains("manifest_models") || !m["manifest_models"].is_object()) continue;
        for (auto it = m["manifest_models"].begin(); it != m["manifest_models"].end(); ++it) {
            manifest["models"][it.key()] = it.value();
        }
    }
    std::string dump = manifest.dump(2) + "\n";
    HostHD::write_file_bytes(manifest_path, reinterpret_cast<const uint8_t *>(dump.data()), dump.size());
}

std::unordered_set<std::string> collect_other_enabled_files(const nlohmann::json &doc, const std::string &skip_id,
                                                            const char *bucket, const std::string &dir) {
    std::unordered_set<std::string> used;
    for (const auto &m : doc["mods"]) {
        if (!m.is_object() || !m.value("enabled", true)) continue;
        if (m.value("id", "") == skip_id || !registry_matches_root(m, bucket, dir)) continue;
        if (m.contains("files") && m["files"].is_object() && m["files"].contains(bucket) && m["files"][bucket].is_array()) {
            for (const auto &f : m["files"][bucket]) {
                if (f.is_string()) used.insert(f.get<std::string>());
            }
        }
    }
    return used;
}
} // namespace

extern "C" int host_mod_registry_set_enabled(const char *data_dir, const char *mod_id, int enabled,
                                             const char *textures_dir, const char *models_dir,
                                             const char *movies_dir) {
    if (!data_dir || !data_dir[0] || !mod_id || !mod_id[0]) return 0;
    std::string id(mod_id);
    if (!HostHD::is_safe_rel_path(id) || id.find('/') != std::string::npos) return 0;
    namespace fs = std::filesystem;
    fs::path reg_path = fs::path(data_dir) / "mods-registry.json";
    nlohmann::json doc;
    if (!HostHD::load_registry_json(reg_path, doc)) return 0;

    nlohmann::json *target = nullptr;
    for (auto &m : doc["mods"]) {
        if (m.is_object() && m.value("id", "") == id) { target = &m; break; }
    }
    if (!target) return 0;
    bool want = enabled != 0;
    if (target->value("enabled", true) == want) return 1;

    fs::path disabled_root = fs::path(data_dir) / "mods-disabled" / id;
    std::string tex_d = (textures_dir && textures_dir[0]) ? textures_dir : (fs::path(data_dir) / "textures-hd").string();
    std::string mod_d = (models_dir && models_dir[0]) ? models_dir : (fs::path(data_dir) / "models-hd").string();
    std::string mov_d = (movies_dir && movies_dir[0]) ? movies_dir : (fs::path(data_dir) / "movies-hd").string();
    std::string aud_d = (fs::path(data_dir) / "audio-hd").string();

    struct Bucket { const char *name; std::string dir; };
    Bucket buckets[] = {{"textures", tex_d}, {"models", mod_d}, {"movies", mov_d}, {"audio", aud_d}};
    if (target->contains("files") && (*target)["files"].is_object()) {
        std::vector<std::pair<fs::path, fs::path>> moves;
        for (const auto &b : buckets) {
            const auto &files = (*target)["files"];
            if (!files.contains(b.name) || !files[b.name].is_array() || files[b.name].empty()) continue;
            if (!registry_matches_root(*target, b.name, b.dir)) return 0;
            auto other_used = collect_other_enabled_files(doc, id, b.name, b.dir);
            for (const auto &f : files[b.name]) {
                if (!f.is_string()) continue;
                std::string rel = f.get<std::string>();
                if (!HostHD::is_safe_rel_path(rel)) continue;
                fs::path active_p = fs::path(b.dir) / rel;
                fs::path stash_p = disabled_root / b.name / rel;
                if (want) moves.emplace_back(stash_p, active_p);
                else if (!other_used.count(rel)) moves.emplace_back(active_p, stash_p);
            }
        }
        for (const auto &[src, dst] : moves)
            if (HostHD::path_occupied(src) && HostHD::path_occupied(dst)) return 0;
        for (const auto &[src, dst] : moves)
            if (!HostHD::move_file_safe(src, dst)) return 0;
    }
    (*target)["enabled"] = want;
    if (!HostHD::save_registry_json(reg_path, doc)) return 0;
    if (target->contains("manifest_models") && !(*target)["manifest_models"].empty()) {
        rebuild_models_manifest_from_registry(doc, mod_d);
    }
    if (want) {
        std::error_code ec;
        fs::remove_all(disabled_root, ec);
    }
    return 1;
}

extern "C" int host_mod_registry_remove(const char *data_dir, const char *mod_id,
                                        const char *textures_dir, const char *models_dir,
                                        const char *movies_dir) {
    if (!data_dir || !data_dir[0] || !mod_id || !mod_id[0]) return 0;
    std::string id(mod_id);
    if (!HostHD::is_safe_rel_path(id) || id.find('/') != std::string::npos) return 0;
    namespace fs = std::filesystem;
    fs::path reg_path = fs::path(data_dir) / "mods-registry.json";
    nlohmann::json doc;
    if (!HostHD::load_registry_json(reg_path, doc)) return 0;

    size_t found_idx = SIZE_MAX;
    for (size_t i = 0; i < doc["mods"].size(); ++i) {
        if (doc["mods"][i].is_object() && doc["mods"][i].value("id", "") == id) {
            found_idx = i;
            break;
        }
    }
    if (found_idx == SIZE_MAX) return 0;

    if (doc["mods"][found_idx].value("enabled", true)) {
        if (!host_mod_registry_set_enabled(data_dir, mod_id, 0, textures_dir, models_dir, movies_dir)) return 0;
        if (!HostHD::load_registry_json(reg_path, doc)) return 0;
    }
    doc["mods"].erase(doc["mods"].begin() + std::ptrdiff_t(found_idx));
    if (!HostHD::save_registry_json(reg_path, doc)) return 0;
    std::error_code ec;
    fs::remove_all(fs::path(data_dir) / "mods-disabled" / id, ec);
    return !ec;
}
