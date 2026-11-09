// Compare the complete integer transform with an independent wide oracle.
#include "equiv.h"
#include "lh_test.h"
#include "oracles/media.h"
#include "py_random.h"
#include "values.h"

#include <array>
#include <cstdint>
#include <vector>

namespace {
using Block = std::array<int16_t, 64>;

Block filled(int16_t value) {
    Block block;
    block.fill(value);
    return block;
}

Block random_block(lh::PyRandom &rng) {
    Block block;
    for (int16_t &value : block) value = static_cast<int16_t>(rng.randint(-32768, 32767));
    return block;
}
} // namespace

LH_TEST(test_idct_rounding_and_full_int16_bounds) {
    const auto transform = t.library("mdec").fn<void(int16_t *, const int16_t *)>("mdec_idct");
    lh::PyRandom rng(90210);
    const std::array<int16_t, 7> extremes = {-32768, -32767, -1, 0, 1, 32766, 32767};
    std::vector<std::pair<Block, Block>> vectors;
    for (const int16_t b : extremes) {
        for (const int16_t s : extremes) vectors.emplace_back(filled(b), filled(s));
    }
    for (int i = 0; i < 256; ++i) {
        // Each element draws in Python's evaluation order: block, then scale.
        Block block = random_block(rng), scale = random_block(rng);
        vectors.emplace_back(block, scale);
    }
    for (int i = 0; i < 256; ++i) {
        // Sparse coefficient blocks exercise the zero-column shortcut in both passes.
        Block block{};
        for (int16_t &value : block) value = rng.random() < 0.1 ? static_cast<int16_t>(rng.randint(-32768, 32767)) : 0;
        Block scale = random_block(rng);
        vectors.emplace_back(block, scale);
    }
    for (const auto &[block, scale] : vectors) {
        Block values = block;
        transform(values.data(), scale.data());
        const Block expected = lh::oracle::idct_reference(block, scale);
        lh::equiv::record(t, block, scale, expected);
        LH_SEQ_EQ(values, expected, "");
    }
}
