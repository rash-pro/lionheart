// Independent SHA-1 message schedule and compression, including bit-length padding.
#pragma once
#include <array>
#include <cstdint>
#include <string>

namespace lh::oracle {
inline std::string sha1(std::string message) {
    const uint64_t bits = uint64_t(message.size()) * 8;
    message.push_back(char(0x80));
    while (message.size() % 64 != 56) message.push_back(0);
    for (int i = 7; i >= 0; --i) message.push_back(char(bits >> (8 * i)));
    std::array<uint32_t, 5> h = {0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476, 0xc3d2e1f0};
    const auto rol = [](uint32_t x, unsigned n) { return (x << n) | (x >> (32 - n)); };
    for (size_t offset = 0; offset < message.size(); offset += 64) {
        std::array<uint32_t, 80> w{};
        for (unsigned i = 0; i < 16; ++i)
            for (unsigned j = 0; j < 4; ++j) w[i] = (w[i] << 8) | uint8_t(message[offset + 4*i + j]);
        for (unsigned i = 16; i < 80; ++i) w[i] = rol(w[i-3] ^ w[i-8] ^ w[i-14] ^ w[i-16], 1);
        auto a=h[0], b=h[1], c=h[2], d=h[3], e=h[4];
        for (unsigned i = 0; i < 80; ++i) {
            const uint32_t f = i < 20 ? (b & c) | (~b & d) : i < 40 ? b ^ c ^ d :
                               i < 60 ? (b & c) | (b & d) | (c & d) : b ^ c ^ d;
            const uint32_t k = i < 20 ? 0x5a827999 : i < 40 ? 0x6ed9eba1 : i < 60 ? 0x8f1bbcdc : 0xca62c1d6;
            const uint32_t next = rol(a, 5) + f + e + k + w[i];
            e=d; d=c; c=rol(b, 30); b=a; a=next;
        }
        h[0]+=a; h[1]+=b; h[2]+=c; h[3]+=d; h[4]+=e;
    }
    std::string out;
    for (auto word : h) for (int i = 7; i >= 0; --i) out += "0123456789abcdef"[(word >> (4*i)) & 15];
    return out;
}
}
