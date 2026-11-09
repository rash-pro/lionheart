// Exact timing reciprocals, including the high-word carry and correction cases.
#include "equiv.h"
#include "lh_test.h"
#include "py_random.h"
#include "values.h"

#include <array>
#include <cstdint>
#include <vector>

__extension__ typedef unsigned __int128 uint128;

LH_TEST(test_cycle_reciprocals) {
    const std::array<uint64_t, 8> divisors = {263, 314, 526, 628, 62800, 63120000, 33868800, 33868800ull * 1001};
    const auto lib = t.library("ratios");
    const auto ratio = lib.fn<uint64_t(uint64_t, unsigned)>("ratio");
    const auto product = lib.fn<uint64_t(uint64_t, uint64_t)>("product");
    lh::PyRandom rng(8);
    const uint64_t mask = ~uint64_t(0);
    for (unsigned i = 0; i < divisors.size(); ++i) {
        const uint64_t d = divisors[i];
        std::vector<uint64_t> values = {0, 1, d - 1, d, d + 1, mask, mask - 1, 0xffffffffull, 0x100000000ull};
        for (uint64_t k : {uint64_t(1), uint64_t(2), uint64_t(65535), uint64_t(0xffffffffull), mask / d}) {
            for (const uint128 n : {uint128(k) * d - 1, uint128(k) * d, uint128(k) * d + 1}) {
                if (n <= uint128(mask)) values.push_back(static_cast<uint64_t>(n));
            }
        }
        for (int n = 0; n < 10000; ++n) values.push_back(rng.getrandbits(64));
        for (const uint64_t n : values) {
            lh::equiv::record(t, n, i, n / d);
            LH_CHECK_MSG(ratio(n, i) == n / d, lh::str("n=", n, " d=", d, " actual=", ratio(n, i), " expected=", n / d));
        }
    }
    for (int n = 0; n < 10000; ++n) {
        const uint64_t a = rng.getrandbits(64), b = rng.getrandbits(64);
        lh::equiv::record(t, a, b, a * b);
        LH_CHECK_MSG(product(a, b) == a * b, lh::str("a=", a, " b=", b, " actual=", product(a, b), " expected=", a * b));
    }
}
