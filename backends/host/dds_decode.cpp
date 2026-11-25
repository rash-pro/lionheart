#include "dds_decode.hpp"

#include <algorithm>
#include <cstring>

namespace HostHD {
namespace {

inline uint16_t le16(const uint8_t *p) {
    return uint16_t(p[0]) | uint16_t(uint16_t(p[1]) << 8);
}

inline uint32_t le32(const uint8_t *p) {
    return uint32_t(p[0]) | (uint32_t(p[1]) << 8) |
           (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
}

constexpr uint32_t fourcc(char a, char b, char c, char d) {
    return uint32_t(uint8_t(a)) | (uint32_t(uint8_t(b)) << 8) |
           (uint32_t(uint8_t(c)) << 16) | (uint32_t(uint8_t(d)) << 24);
}

inline void rgb565_to_rgb8(uint16_t c, uint8_t &r, uint8_t &g, uint8_t &b) {
    uint8_t r5 = uint8_t((c >> 11) & 31u);
    uint8_t g6 = uint8_t((c >> 5) & 63u);
    uint8_t b5 = uint8_t(c & 31u);
    r = uint8_t((r5 << 3) | (r5 >> 2));
    g = uint8_t((g6 << 2) | (g6 >> 4));
    b = uint8_t((b5 << 3) | (b5 >> 2));
}

inline void decode_bc1_block(const uint8_t *blk, uint8_t out_rgba[16][4], bool allow_transparent) {
    uint16_t c0 = le16(blk + 0);
    uint16_t c1 = le16(blk + 2);
    uint32_t bits = le32(blk + 4);
    uint8_t pal[4][4];
    rgb565_to_rgb8(c0, pal[0][0], pal[0][1], pal[0][2]);
    pal[0][3] = 255;
    rgb565_to_rgb8(c1, pal[1][0], pal[1][1], pal[1][2]);
    pal[1][3] = 255;

    if (!allow_transparent || c0 > c1) {
        for (int ch = 0; ch < 3; ++ch) {
            pal[2][ch] = uint8_t((2u * pal[0][ch] + pal[1][ch] + 1u) / 3u);
            pal[3][ch] = uint8_t((pal[0][ch] + 2u * pal[1][ch] + 1u) / 3u);
        }
        pal[2][3] = 255;
        pal[3][3] = 255;
    } else {
        for (int ch = 0; ch < 3; ++ch) {
            pal[2][ch] = uint8_t((unsigned(pal[0][ch]) + unsigned(pal[1][ch])) >> 1);
            pal[3][ch] = 0;
        }
        pal[2][3] = 255;
        pal[3][3] = 0;
    }

    for (int i = 0; i < 16; ++i) {
        uint32_t idx = (bits >> (2 * i)) & 3u;
        std::memcpy(out_rgba[i], pal[idx], 4);
    }
}

inline void decode_bc2_block(const uint8_t *blk, uint8_t out_rgba[16][4]) {
    decode_bc1_block(blk + 8, out_rgba, false);
    for (int i = 0; i < 16; ++i) {
        uint8_t byte = blk[i >> 1];
        uint8_t a4 = (i & 1) ? uint8_t(byte >> 4) : uint8_t(byte & 0xfu);
        out_rgba[i][3] = uint8_t((a4 << 4) | a4);
    }
}

inline void decode_bc3_block(const uint8_t *blk, uint8_t out_rgba[16][4]) {
    decode_bc1_block(blk + 8, out_rgba, false);
    uint8_t a[8];
    a[0] = blk[0];
    a[1] = blk[1];
    if (a[0] > a[1]) {
        for (int k = 1; k <= 6; ++k) {
            a[1 + k] = uint8_t(((7 - k) * unsigned(a[0]) + k * unsigned(a[1]) + 3u) / 7u);
        }
    } else {
        for (int k = 1; k <= 4; ++k) {
            a[1 + k] = uint8_t(((5 - k) * unsigned(a[0]) + k * unsigned(a[1]) + 2u) / 5u);
        }
        a[6] = 0;
        a[7] = 255;
    }
    uint64_t abits = 0;
    for (int i = 0; i < 6; ++i) abits |= uint64_t(blk[2 + i]) << (8 * i);
    for (int i = 0; i < 16; ++i) {
        out_rgba[i][3] = a[(abits >> (3 * i)) & 7u];
    }
}

/* ---------- Khronos Data Format Specification §22 (BPTC / BC7) ---------- */

struct BitReader128 {
    uint64_t lo = 0;
    uint64_t hi = 0;
    unsigned pos = 0;

    explicit BitReader128(const uint8_t *blk) {
        for (int i = 0; i < 8; ++i) {
            lo |= uint64_t(blk[i]) << (8 * i);
            hi |= uint64_t(blk[8 + i]) << (8 * i);
        }
    }

    uint32_t read(unsigned n) {
        if (!n) return 0;
        uint32_t val = 0;
        if (pos < 64) {
            unsigned avail = 64u - pos;
            if (n <= avail) {
                val = uint32_t((lo >> pos) & ((UINT64_C(1) << n) - 1u));
            } else {
                uint64_t low_part = lo >> pos;
                unsigned rem = n - avail;
                uint64_t high_part = hi & ((UINT64_C(1) << rem) - 1u);
                val = uint32_t(low_part | (high_part << avail));
            }
        } else {
            unsigned shift = pos - 64u;
            val = uint32_t((hi >> shift) & ((UINT64_C(1) << n) - 1u));
        }
        pos += n;
        return val;
    }
};

/* BC7 2-subset partition table: 64 partitions, 16 pixels packed as 1 bit per pixel (LSB = pixel 0). */
constexpr uint16_t kBc7P2[64] = {
    0xcccc, 0x8888, 0xeeee, 0xecc8, 0xc880, 0xfeec, 0xfec8, 0xec80,
    0xc800, 0xffec, 0xfe80, 0xe800, 0xffe8, 0xff00, 0xfff0, 0xf000,
    0xf710, 0x008e, 0x7100, 0x08ce, 0x008c, 0x7310, 0x3100, 0x8cce,
    0x088c, 0x3110, 0x6666, 0x366c, 0x17e8, 0x0ff0, 0x718e, 0x399c,
    0xaaaa, 0xf0f0, 0x5a5a, 0x33cc, 0x3c3c, 0x55aa, 0x9696, 0xa55a,
    0x73ce, 0x13c8, 0x324c, 0x3bdc, 0x6996, 0xc33c, 0x9966, 0x0660,
    0x0272, 0x04e4, 0x4e40, 0x2720, 0xc936, 0x936c, 0x39c6, 0x639c,
    0x9336, 0x9cc6, 0x817e, 0xe718, 0xccf0, 0x0fcc, 0x7744, 0xee22
};

/* BC7 3-subset partition table: 64 partitions, 16 pixels packed as 2 bits per pixel (bits 2*i..2*i+1). */
constexpr uint32_t kBc7P3[64] = {
    0xaa685050u, 0x6a5a5040u, 0x5a5a4200u, 0x5450a0a8u, 0xa5a50000u, 0xa0a05050u, 0x5555a0a0u, 0x5a5a5050u,
    0xaa550000u, 0xaa555500u, 0xaaaa5500u, 0x90909090u, 0x94949494u, 0xa4a4a4a4u, 0xa9a59450u, 0x2a0a4250u,
    0xa5945040u, 0x0a425054u, 0xa5a5a500u, 0x55a0a0a0u, 0xa8a85454u, 0x6a6a4040u, 0xa4a45000u, 0x1a1a0500u,
    0x0050a4a4u, 0xaaa59090u, 0x14696914u, 0x69691400u, 0xa08585a0u, 0xaa821414u, 0x50a4a450u, 0x6a5a0200u,
    0xa9a58000u, 0x5090a0a8u, 0xa8a09050u, 0x24242424u, 0x00aa5500u, 0x24924924u, 0x24499224u, 0x50a50a50u,
    0x500aa550u, 0xaaaa4444u, 0x66660000u, 0xa5a0a5a0u, 0x50a050a0u, 0x69286928u, 0x44aaaa44u, 0x66666600u,
    0xaa444444u, 0x54a854a8u, 0x95809580u, 0x96969600u, 0xa85454a8u, 0x80959580u, 0xaa141414u, 0x96960000u,
    0xaaaa1414u, 0xa05050a0u, 0xa0a5a5a0u, 0x96000000u, 0x40804080u, 0xa9a8a9a8u, 0xaaaaaa44u, 0x2a4a5254u
};

/* Anchor pixel index for subset 1 of 2-subset partitions. */
constexpr uint8_t kBc7A2[64] = {
    15, 15, 15, 15, 15, 15, 15, 15,
    15, 15, 15, 15, 15, 15, 15, 15,
    15,  2,  8,  2,  2,  8,  8, 15,
     2,  8,  2,  2,  8,  8,  2,  2,
    15, 15,  6,  8,  2,  8, 15, 15,
     2,  8,  2,  2,  2, 15, 15,  6,
     6,  2,  6,  8, 15, 15,  2,  2,
    15, 15, 15, 15, 15,  2,  2, 15
};

/* Anchor pixel index for subset 1 of 3-subset partitions. */
constexpr uint8_t kBc7A3a[64] = {
     3,  3, 15, 15,  8,  3, 15, 15,
     8,  8,  6,  6,  6,  5,  3,  3,
     3,  3,  8, 15,  3,  3,  6, 10,
     5,  8,  8,  6,  8,  5, 15, 15,
     8, 15,  3,  5,  6, 10,  8, 15,
    15,  3, 15,  5, 15, 15, 15, 15,
     3, 15,  5,  5,  5,  8,  5, 10,
     5, 10,  8, 13, 15, 12,  3,  3
};

/* Anchor pixel index for subset 2 of 3-subset partitions. */
constexpr uint8_t kBc7A3b[64] = {
    15,  8,  8,  3, 15, 15,  3,  8,
    15, 15, 15, 15, 15, 15, 15,  8,
    15,  8, 15,  3, 15,  8, 15,  8,
     3, 15,  6, 10, 15, 15, 10,  8,
    15,  3, 15, 10, 10,  8,  9, 10,
     6, 15,  8, 15,  3,  6,  6,  8,
    15,  3, 15, 15, 15, 15, 15, 15,
    15, 15, 15, 15,  3, 15, 15,  8
};

constexpr uint8_t kBc7W2[4] = {0, 21, 43, 64};
constexpr uint8_t kBc7W3[8] = {0, 9, 18, 27, 37, 46, 55, 64};
constexpr uint8_t kBc7W4[16] = {
    0, 4, 9, 13, 17, 21, 26, 30, 34, 38, 43, 47, 51, 55, 60, 64
};

struct Bc7ModeInfo {
    uint8_t ns;   /* number of subsets (1..3) */
    uint8_t pb;   /* partition bits */
    uint8_t rb;   /* rotation bits */
    uint8_t isb;  /* index selection bits */
    uint8_t cb;   /* color channel bits */
    uint8_t ab;   /* alpha channel bits */
    uint8_t epb;  /* unique endpoint p-bits */
    uint8_t spb;  /* shared subset p-bits */
    uint8_t ib;   /* primary index bits */
    uint8_t ib2;  /* secondary index bits */
};

constexpr Bc7ModeInfo kBc7Modes[8] = {
    {3, 4, 0, 0, 4, 0, 1, 0, 3, 0}, /* Mode 0 */
    {2, 6, 0, 0, 6, 0, 0, 1, 3, 0}, /* Mode 1 */
    {3, 6, 0, 0, 5, 0, 0, 0, 2, 0}, /* Mode 2 */
    {2, 6, 0, 0, 7, 0, 1, 0, 2, 0}, /* Mode 3 */
    {1, 0, 2, 1, 5, 6, 0, 0, 2, 3}, /* Mode 4 */
    {1, 0, 2, 0, 7, 8, 0, 0, 2, 2}, /* Mode 5 */
    {1, 0, 0, 0, 7, 7, 1, 0, 4, 0}, /* Mode 6 */
    {2, 6, 0, 0, 5, 5, 1, 0, 2, 0}, /* Mode 7 */
};

inline uint8_t bc7_unquant(uint32_t v, unsigned prec) {
    return uint8_t((v << (8u - prec)) | (v >> (2u * prec - 8u)));
}

inline uint8_t bc7_weight(unsigned bits, uint32_t idx) {
    if (bits == 2) return kBc7W2[idx & 3u];
    if (bits == 3) return kBc7W3[idx & 7u];
    return kBc7W4[idx & 15u];
}

inline uint8_t bc7_interp(uint8_t e0, uint8_t e1, uint8_t w) {
    return uint8_t(((64u - w) * unsigned(e0) + w * unsigned(e1) + 32u) >> 6);
}

inline void decode_bc7_block(const uint8_t *blk, uint8_t out_rgba[16][4]) {
    BitReader128 br(blk);
    unsigned mode = 0;
    while (mode < 8 && br.read(1) == 0) ++mode;
    if (mode >= 8) {
        /* Reserved mode: Khronos spec defines all-zero RGBA output. */
        std::memset(out_rgba, 0, 16 * 4);
        return;
    }

    const Bc7ModeInfo &m = kBc7Modes[mode];
    const uint32_t part = m.pb ? br.read(m.pb) : 0u;
    const uint32_t rot = m.rb ? br.read(m.rb) : 0u;
    const uint32_t isb = m.isb ? br.read(m.isb) : 0u;

    uint32_t ep[3][2][4] = {};
    for (unsigned ch = 0; ch < 3; ++ch) {
        for (unsigned s = 0; s < m.ns; ++s) {
            ep[s][0][ch] = br.read(m.cb);
            ep[s][1][ch] = br.read(m.cb);
        }
    }
    if (m.ab) {
        for (unsigned s = 0; s < m.ns; ++s) {
            ep[s][0][3] = br.read(m.ab);
            ep[s][1][3] = br.read(m.ab);
        }
    }

    uint8_t q[3][2][4] = {};
    if (m.epb) {
        for (unsigned s = 0; s < m.ns; ++s) {
            for (unsigned e = 0; e < 2; ++e) {
                uint32_t p = br.read(1);
                for (unsigned ch = 0; ch < 3; ++ch) {
                    q[s][e][ch] = bc7_unquant((ep[s][e][ch] << 1) | p, m.cb + 1u);
                }
                q[s][e][3] = m.ab ? bc7_unquant((ep[s][e][3] << 1) | p, m.ab + 1u) : 255u;
            }
        }
    } else if (m.spb) {
        for (unsigned s = 0; s < m.ns; ++s) {
            uint32_t p = br.read(1);
            for (unsigned e = 0; e < 2; ++e) {
                for (unsigned ch = 0; ch < 3; ++ch) {
                    q[s][e][ch] = bc7_unquant((ep[s][e][ch] << 1) | p, m.cb + 1u);
                }
                q[s][e][3] = 255u;
            }
        }
    } else {
        for (unsigned s = 0; s < m.ns; ++s) {
            for (unsigned e = 0; e < 2; ++e) {
                for (unsigned ch = 0; ch < 3; ++ch) {
                    q[s][e][ch] = bc7_unquant(ep[s][e][ch], m.cb);
                }
                q[s][e][3] = m.ab ? bc7_unquant(ep[s][e][3], m.ab) : 255u;
            }
        }
    }

    uint8_t subset[16];
    uint8_t anchor1 = 255, anchor2 = 255;
    if (m.ns == 1) {
        std::memset(subset, 0, sizeof(subset));
    } else if (m.ns == 2) {
        uint16_t mask = kBc7P2[part & 63u];
        anchor1 = kBc7A2[part & 63u];
        for (unsigned i = 0; i < 16; ++i) subset[i] = uint8_t((mask >> i) & 1u);
    } else {
        uint32_t mask = kBc7P3[part & 63u];
        anchor1 = kBc7A3a[part & 63u];
        anchor2 = kBc7A3b[part & 63u];
        for (unsigned i = 0; i < 16; ++i) subset[i] = uint8_t((mask >> (2u * i)) & 3u);
    }

    uint8_t idx1[16];
    uint8_t idx2[16] = {};
    for (unsigned i = 0; i < 16; ++i) {
        bool is_anchor = (i == 0) || (i == anchor1) || (i == anchor2);
        idx1[i] = uint8_t(br.read(is_anchor ? (m.ib - 1u) : m.ib));
    }
    if (m.ib2) {
        for (unsigned i = 0; i < 16; ++i) {
            bool is_anchor = (i == 0);
            idx2[i] = uint8_t(br.read(is_anchor ? (m.ib2 - 1u) : m.ib2));
        }
    }

    for (unsigned i = 0; i < 16; ++i) {
        unsigned s = subset[i];
        uint8_t cw, aw;
        if (!m.ib2) {
            cw = aw = bc7_weight(m.ib, idx1[i]);
        } else if (isb == 0) {
            cw = bc7_weight(m.ib, idx1[i]);
            aw = bc7_weight(m.ib2, idx2[i]);
        } else {
            cw = bc7_weight(m.ib2, idx2[i]);
            aw = bc7_weight(m.ib, idx1[i]);
        }
        uint8_t r = bc7_interp(q[s][0][0], q[s][1][0], cw);
        uint8_t g = bc7_interp(q[s][0][1], q[s][1][1], cw);
        uint8_t b = bc7_interp(q[s][0][2], q[s][1][2], cw);
        uint8_t a = m.ab ? bc7_interp(q[s][0][3], q[s][1][3], aw) : 255u;

        if (rot == 1) std::swap(r, a);
        else if (rot == 2) std::swap(g, a);
        else if (rot == 3) std::swap(b, a);

        out_rgba[i][0] = r;
        out_rgba[i][1] = g;
        out_rgba[i][2] = b;
        out_rgba[i][3] = a;
    }
}

template <typename BlockFn>
bool decode_blocks(const uint8_t *payload, size_t payload_size, uint32_t w, uint32_t h,
                   size_t block_bytes, DdsImage &out, BlockFn &&fn, std::string *error) {
    uint64_t bx = (uint64_t(w) + 3u) / 4u;
    uint64_t by = (uint64_t(h) + 3u) / 4u;
    uint64_t need = bx * by * block_bytes;
    if (payload_size < need) {
        if (error) *error = "truncated DDS compressed payload";
        return false;
    }
    out.width = w;
    out.height = h;
    out.rgba.resize(size_t(w) * size_t(h) * 4u);

    uint8_t block_rgba[16][4];
    const uint8_t *src = payload;
    for (uint64_t y = 0; y < by; ++y) {
        for (uint64_t x = 0; x < bx; ++x) {
            fn(src, block_rgba);
            src += block_bytes;
            for (unsigned py = 0; py < 4; ++py) {
                uint32_t dy = uint32_t(y * 4u + py);
                if (dy >= h) continue;
                for (unsigned px = 0; px < 4; ++px) {
                    uint32_t dx = uint32_t(x * 4u + px);
                    if (dx >= w) continue;
                    uint8_t *dst = out.rgba.data() + (size_t(dy) * w + dx) * 4u;
                    std::memcpy(dst, block_rgba[py * 4u + px], 4);
                }
            }
        }
    }
    return true;
}

inline unsigned mask_shift(uint32_t mask) {
    if (!mask) return 0;
    unsigned s = 0;
    while ((mask & 1u) == 0u) {
        mask >>= 1;
        ++s;
    }
    return s;
}

inline uint8_t extract_masked(uint32_t px, uint32_t mask) {
    if (!mask) return 255u;
    unsigned s = mask_shift(mask);
    uint32_t max_v = mask >> s;
    if (!max_v) return 255u;
    uint32_t v = (px & mask) >> s;
    if (max_v == 255u) return uint8_t(v);
    return uint8_t((v * 255u + max_v / 2u) / max_v);
}

} // namespace

bool decode_dds(const uint8_t *data, size_t size, DdsImage &out, std::string *error) {
    if (!data || size < 128) {
        if (error) *error = "DDS file too small";
        return false;
    }
    if (std::memcmp(data, "DDS ", 4) != 0) {
        if (error) *error = "invalid DDS magic";
        return false;
    }
    uint32_t hdr_size = le32(data + 4);
    if (hdr_size != 124) {
        if (error) *error = "unexpected DDS header size";
        return false;
    }
    uint32_t height = le32(data + 12);
    uint32_t width = le32(data + 16);
    if (width == 0 || height == 0 || width > 16384 || height > 16384) {
        if (error) *error = "invalid DDS dimensions";
        return false;
    }

    const uint8_t *pf = data + 76;
    uint32_t pf_flags = le32(pf + 4);
    uint32_t pf_fourcc = le32(pf + 8);
    uint32_t rgb_bits = le32(pf + 12);
    uint32_t r_mask = le32(pf + 16);
    uint32_t g_mask = le32(pf + 20);
    uint32_t b_mask = le32(pf + 24);
    uint32_t a_mask = le32(pf + 28);

    const uint8_t *payload = data + 128;
    size_t payload_size = size - 128;

    constexpr uint32_t kDdpfFourcc = 0x04u;
    constexpr uint32_t kDdpfRgb = 0x40u;

    if (pf_flags & kDdpfFourcc) {
        if (pf_fourcc == fourcc('D', 'X', 'T', '1')) {
            return decode_blocks(payload, payload_size, width, height, 8, out,
                                 [](const uint8_t *blk, uint8_t rgba[16][4]) {
                                     decode_bc1_block(blk, rgba, true);
                                 },
                                 error);
        }
        if (pf_fourcc == fourcc('D', 'X', 'T', '3')) {
            return decode_blocks(payload, payload_size, width, height, 16, out,
                                 decode_bc2_block, error);
        }
        if (pf_fourcc == fourcc('D', 'X', 'T', '5')) {
            return decode_blocks(payload, payload_size, width, height, 16, out,
                                 decode_bc3_block, error);
        }
        if (pf_fourcc == fourcc('D', 'X', '1', '0')) {
            if (payload_size < 20) {
                if (error) *error = "truncated DDS DX10 header";
                return false;
            }
            uint32_t dxgi = le32(payload + 0);
            payload += 20;
            payload_size -= 20;
            /* DXGI_FORMAT values:
             * 28/29 = R8G8B8A8_UNORM(_SRGB), 87/91 = B8G8R8A8_UNORM(_SRGB)
             * 71/72 = BC1, 74/75 = BC2, 77/78 = BC3, 98/99 = BC7 */
            if (dxgi == 71 || dxgi == 72) {
                return decode_blocks(payload, payload_size, width, height, 8, out,
                                     [](const uint8_t *blk, uint8_t rgba[16][4]) {
                                         decode_bc1_block(blk, rgba, true);
                                     },
                                     error);
            }
            if (dxgi == 74 || dxgi == 75) {
                return decode_blocks(payload, payload_size, width, height, 16, out,
                                     decode_bc2_block, error);
            }
            if (dxgi == 77 || dxgi == 78) {
                return decode_blocks(payload, payload_size, width, height, 16, out,
                                     decode_bc3_block, error);
            }
            if (dxgi == 98 || dxgi == 99) {
                return decode_blocks(payload, payload_size, width, height, 16, out,
                                     decode_bc7_block, error);
            }
            if (dxgi == 28 || dxgi == 29 || dxgi == 87 || dxgi == 91) {
                size_t need = size_t(width) * size_t(height) * 4u;
                if (payload_size < need) {
                    if (error) *error = "truncated uncompressed DX10 DDS payload";
                    return false;
                }
                out.width = width;
                out.height = height;
                out.rgba.resize(need);
                if (dxgi == 28 || dxgi == 29) {
                    std::memcpy(out.rgba.data(), payload, need);
                } else {
                    for (size_t i = 0; i < need; i += 4) {
                        out.rgba[i + 0] = payload[i + 2];
                        out.rgba[i + 1] = payload[i + 1];
                        out.rgba[i + 2] = payload[i + 0];
                        out.rgba[i + 3] = payload[i + 3];
                    }
                }
                return true;
            }
            if (error) *error = "unsupported DX10 DXGI_FORMAT=" + std::to_string(dxgi);
            return false;
        }
        if (error) *error = "unsupported DDS FourCC";
        return false;
    }

    if ((pf_flags & kDdpfRgb) && (rgb_bits == 32 || rgb_bits == 24)) {
        size_t bpp = rgb_bits / 8u;
        size_t npix = size_t(width) * size_t(height);
        if (payload_size < npix * bpp) {
            if (error) *error = "truncated uncompressed DDS payload";
            return false;
        }
        out.width = width;
        out.height = height;
        out.rgba.resize(npix * 4u);
        for (size_t i = 0; i < npix; ++i) {
            const uint8_t *p = payload + i * bpp;
            uint32_t px = uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) |
                          (bpp == 4 ? (uint32_t(p[3]) << 24) : 0u);
            out.rgba[i * 4 + 0] = extract_masked(px, r_mask);
            out.rgba[i * 4 + 1] = extract_masked(px, g_mask);
            out.rgba[i * 4 + 2] = extract_masked(px, b_mask);
            out.rgba[i * 4 + 3] = a_mask ? extract_masked(px, a_mask) : 255u;
        }
        return true;
    }

    if (error) *error = "unsupported DDS pixel format";
    return false;
}

} // namespace HostHD
