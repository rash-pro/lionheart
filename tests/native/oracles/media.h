// Independent references for the MDEC inverse transform and CD-XA ADPCM.
// Written from the documented equations; they deliberately share no code with
// runtime/mdec.c or runtime/cdrom.c, which they exist to judge.
#pragma once

#include "numeric.h"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace lh::oracle {

inline std::array<int16_t, 64> idct_reference(const std::array<int16_t, 64> &block, const std::array<int16_t, 64> &scale) {
    std::array<int64_t, 64> current{};
    for (size_t i = 0; i < 64; ++i) current[i] = block[i];
    for (int pass = 0; pass < 2; ++pass) {
        std::array<int64_t, 64> next{};
        for (size_t row = 0; row < 8; ++row) {
            for (size_t x = 0; x < 8; ++x) {
                int64_t sum = 0;
                for (size_t k = 0; k < 8; ++k) sum += current[row + k * 8] * floor_div(scale[x + k * 8], 8);
                next[row * 8 + x] = clip(floor_div(sum + 0x4000, 32768));
            }
        }
        current = next;
    }
    std::array<int16_t, 64> out{};
    for (size_t i = 0; i < 64; ++i) {
        int64_t wrapped = (((current[i] + 256) % 512) + 512) % 512 - 256;
        out[i] = static_cast<int16_t>(clip(wrapped, -128, 127));
    }
    return out;
}

inline std::string xa_sector(unsigned bits, bool stereo, bool half_rate = false) {
    std::string raw(2352, '\0');
    const unsigned char header[] = {2, 1, 2, 0x64,
                                    static_cast<unsigned char>((bits == 8) * 16 + stereo + half_rate * 4),
                                    1, 2, 0x64, 0};
    for (size_t i = 0; i < sizeof(header); ++i) raw[15 + i] = static_cast<char>(header[i]);
    for (unsigned group = 0; group < 18; ++group) {
        const size_t start = 24 + group * 128;
        for (unsigned unit = 0; unit < 32 / bits; ++unit) {
            raw[start + 4 + unit] = static_cast<char>(((group + unit) % 4) * 16 + (group + unit) % 16);
        }
        for (unsigned sample = 0; sample < 28; ++sample) {
            for (unsigned lane = 0; lane < 4; ++lane) {
                raw[start + 16 + sample * 4 + lane] = static_cast<char>((sample * 37 + group * 19 + lane * 67) & 255);
            }
        }
    }
    return raw;
}

using XaHistory = std::array<std::array<int, 2>, 2>;

inline std::vector<int16_t> xa_reference(const std::string &raw, XaHistory &history) {
    static const int kFilterOld[4] = {0, 60, 115, 98}, kFilterOlder[4] = {0, 0, -52, -55};
    const auto header = static_cast<uint8_t>(raw[19]);
    const bool stereo = header & 1;
    const unsigned bits = header & 16 ? 8 : 4, channels = stereo ? 2 : 1;
    std::vector<int16_t> decoded[2];
    for (unsigned group = 0; group < 18; ++group) {
        const auto *p = reinterpret_cast<const uint8_t *>(raw.data()) + 24 + group * 128;
        for (unsigned unit = 0; unit < 32 / bits; ++unit) {
            const unsigned channel = unit % channels;
            unsigned shift = p[4 + unit] & 15u;
            const unsigned predictor = (p[4 + unit] >> 4) & 3u;
            shift = shift > 12 ? 9 : shift;
            for (unsigned i = 0; i < 28; ++i) {
                const uint32_t packed = uint32_t(p[16 + i * 4]) | uint32_t(p[17 + i * 4]) << 8 |
                                        uint32_t(p[18 + i * 4]) << 16 | uint32_t(p[19 + i * 4]) << 24;
                int sample = static_cast<int>((packed >> (unit * bits)) & ((1u << bits) - 1));
                if (sample & (1 << (bits - 1))) sample -= 1 << bits;
                const int old = history[channel][0], older = history[channel][1];
                const int64_t prediction = floor_div(old * kFilterOld[predictor] + older * kFilterOlder[predictor] + 32, 64);
                const auto value = static_cast<int>(clip(floor_div(sample * (int64_t(1) << (16 - bits)), int64_t(1) << shift) + prediction));
                history[channel] = {value, old};
                decoded[channel].push_back(static_cast<int16_t>(value));
            }
        }
    }
    if (!stereo) decoded[1] = decoded[0];
    std::vector<int16_t> interleaved;
    interleaved.reserve(decoded[0].size() * 2);
    for (size_t i = 0; i < decoded[0].size() && i < decoded[1].size(); ++i) {
        interleaved.push_back(decoded[0][i]);
        interleaved.push_back(decoded[1][i]);
    }
    return interleaved;
}

} // namespace lh::oracle
