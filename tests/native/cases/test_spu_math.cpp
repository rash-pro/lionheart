// Mixer coefficient bounds and negative fixed-point rounding stay exact.
#include "lh_test.h"
#include "oracles/spu.h"
#include "py_random.h"
#include "values.h"

#include <array>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

LH_TEST(test_adpcm_matches_integer_reference) {
    const auto decode = t.library("spu").fn<void(const uint8_t *, int16_t *, int16_t *)>("decode");
    lh::PyRandom rng(0xadc0);
    const std::array<std::array<int16_t, 2>, 4> initials = {{{0, 0}, {-32768, 32767}, {32767, -32768}, {1, -1}}};
    for (int header = 0; header < 256; ++header) {
        const std::array<std::string, 4> payloads = {std::string(14, '\0'), std::string(14, '\x88'),
                                                     std::string(14, '\x77'), rng.randbytes(14)};
        for (const std::string &payload : payloads) {
            for (const auto &initial : initials) {
                std::array<uint8_t, 16> data{};
                data[0] = static_cast<uint8_t>(header);
                for (size_t i = 0; i < 14; ++i) data[i + 2] = static_cast<uint8_t>(payload[i]);
                std::array<int16_t, 2> history = initial;
                std::array<int16_t, 28> out{};
                decode(data.data(), history.data(), out.data());
                const auto expected = lh::oracle::adpcm_block(data[0], data.data() + 2, initial[0], initial[1]);
                const std::array<int16_t, 2> expected_history = {expected.history0, expected.history1};

                LH_SEQ_EQ(out, expected.samples, lh::str("header=", header, " initial=", initial[0], ",", initial[1]));
                LH_SEQ_EQ(history, expected_history, lh::str("header=", header));
            }
        }
    }
}

LH_TEST(test_pcm_multiply_matches_wide_reference) {
    const auto lib = t.library("spu");
    const auto coefficient = lib.fn<int32_t(int32_t, int32_t)>("coefficient");
    const auto pcm = lib.fn<int32_t(int32_t, int32_t)>("pcm");
    lh::PyRandom rng(8);
    using Pairs = std::vector<std::pair<int32_t, int32_t>>;
    Pairs pairs;
    for (int a : {-98304, -65536, -32768, -1, 0, 1, 32767, 65535, 98304}) {
        for (int b : {-32768, -1, 0, 1, 32767}) pairs.emplace_back(a, b);
    }
    for (int a : {-32768, -1, 0, 1, 32767}) {
        for (int b : {32768, 65534, 65535}) pairs.emplace_back(a, b);
    }
    for (int i = 0; i < 10000; ++i) {
        const auto a = static_cast<int32_t>(rng.randrange(-98304, 98305));
        const auto b = static_cast<int32_t>(rng.randrange(-32768, 32768));
        pairs.emplace_back(a, b);
    }
    for (int i = 0; i < 10000; ++i) {
        const auto a = static_cast<int32_t>(rng.randrange(-32768, 32768));
        const auto b = static_cast<int32_t>(rng.randrange(65536));
        pairs.emplace_back(a, b);
    }
    for (const auto &[a, b] : pairs) {
        const int32_t expected = lh::oracle::fixed_multiply(a, b);

        LH_CHECK_MSG(coefficient(a, b) == expected, lh::str("a=", a, " b=", b, " actual=", coefficient(a, b), " expected=", expected));
    }
    // Voice multiplication can produce +32768; pitch modulation can reach
    // 65536, but its other operand is explicitly int16_t.
    Pairs voice;
    for (int a : {-32768, -1, 0, 1, 32767, 32768}) {
        for (int b : {-32768, -1, 0, 1, 32767}) voice.emplace_back(a, b);
    }
    for (int a : {-32768, -1, 0, 1, 32767}) {
        for (int b : {32768, 65535, 65536}) voice.emplace_back(a, b);
    }
    for (int i = 0; i < 10000; ++i) {
        const auto a = static_cast<int32_t>(rng.randrange(-32768, 32769));
        const auto b = static_cast<int32_t>(rng.randrange(-32768, 32768));
        voice.emplace_back(a, b);
    }
    for (int i = 0; i < 10000; ++i) {
        const auto a = static_cast<int32_t>(rng.randrange(-32768, 32768));
        const auto b = static_cast<int32_t>(rng.randrange(65537));
        voice.emplace_back(a, b);
    }
    for (const auto &[a, b] : voice) {
        const int32_t expected = lh::oracle::fixed_multiply(a, b);

        LH_CHECK_MSG(pcm(a, b) == expected, lh::str("a=", a, " b=", b, " actual=", pcm(a, b), " expected=", expected));
    }
}
