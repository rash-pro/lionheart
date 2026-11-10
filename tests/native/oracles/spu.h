// Independent reference for SPU ADPCM block decoding and the fixed-point mixers.
// Written from the block format; it deliberately shares no code with
// runtime/spu.c, which it exists to judge.
#pragma once

#include "numeric.h"

#include <array>
#include <cstdint>

namespace lh::oracle {

struct AdpcmBlock {
    std::array<int16_t, 28> samples;
    int16_t history0, history1;
};

inline AdpcmBlock adpcm_block(uint8_t header, const uint8_t payload[14], int16_t history0, int16_t history1) {
    static const int kFilters[5][2] = {{0, 0}, {60, 0}, {115, -52}, {98, -55}, {122, -60}};
    unsigned shift = header & 15u, predictor = header >> 4;
    shift = shift > 12 ? 9 : shift;
    predictor = predictor > 4 ? 0 : predictor;
    const int a = kFilters[predictor][0], b = kFilters[predictor][1];
    int h0 = history0, h1 = history1;
    AdpcmBlock block{};
    for (int i = 0; i < 28; ++i) {
        int nibble = (payload[i / 2] >> ((i & 1) * 4)) & 15;
        if (nibble & 8) nibble -= 16;
        int value = static_cast<int>(floor_shift(nibble * 4096, shift) + floor_shift(h0 * a + h1 * b + 32, 6));
        value = static_cast<int>(clip(value));
        block.samples[static_cast<size_t>(i)] = static_cast<int16_t>(value);
        h1 = h0;
        h0 = value;
    }
    block.history0 = static_cast<int16_t>(h0);
    block.history1 = static_cast<int16_t>(h1);
    return block;
}

// The mixer multiplications, wide enough that neither operand range can overflow.
inline int32_t fixed_multiply(int32_t a, int32_t b) { return static_cast<int32_t>(floor_shift(int64_t(a) * b, 15)); }

} // namespace lh::oracle
