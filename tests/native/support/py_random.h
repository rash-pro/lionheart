// CPython's random.Random for integer seeds, so a native case draws the same
// values a pytest case with the same seed drew. Many of the ported tests make
// tens of thousands of draws, so a few dozen lines here replace megabytes of
// frozen vectors, and later ports get the same inputs by construction.
//
// Only the methods those tests use are here. Each one follows the 3.9-3.14
// implementation exactly: the generator is MT19937 seeded through
// init_by_array with the integer's 32-bit digits, randrange rejection-samples
// from getrandbits, and randbytes is getrandbits read little-endian.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

namespace lh {

class PyRandom {
public:
    explicit PyRandom(uint64_t seed) {
        std::array<uint32_t, 2> key = {static_cast<uint32_t>(seed), static_cast<uint32_t>(seed >> 32)};
        size_t length = key[1] ? 2 : 1;
        init_by_array(key.data(), length);
    }

    uint32_t genrand_uint32() {
        if (index_ >= kN) generate();
        uint32_t y = state_[index_++];
        y ^= y >> 11;
        y ^= (y << 7) & 0x9d2c5680u;
        y ^= (y << 15) & 0xefc60000u;
        y ^= y >> 18;
        return y;
    }

    double random() {
        const uint32_t a = genrand_uint32() >> 5, b = genrand_uint32() >> 6;
        return (a * 67108864.0 + b) * (1.0 / 9007199254740992.0);
    }

    uint64_t getrandbits(unsigned k) {
        if (k == 0) return 0;
        if (k <= 32) return genrand_uint32() >> (32 - k);
        uint64_t value = 0;
        for (unsigned word = 0; k; ++word) {
            uint32_t r = genrand_uint32();
            const unsigned taken = k < 32 ? k : 32;
            if (k < 32) r >>= 32 - k;
            value |= uint64_t(r) << (word * 32);
            k -= taken;
        }
        return value;
    }

    std::string randbytes(size_t n) {
        std::string out;
        out.reserve(n);
        size_t left = n * 8;
        while (left >= 32) {
            uint32_t r = genrand_uint32();
            for (int shift = 0; shift < 32; shift += 8) out.push_back(static_cast<char>(r >> shift));
            left -= 32;
        }
        if (left) {
            uint32_t r = genrand_uint32() >> (32 - left);
            for (size_t i = 0; i < left / 8; ++i) out.push_back(static_cast<char>(r >> (i * 8)));
        }
        return out;
    }

    uint64_t randbelow(uint64_t n) {
        unsigned k = 0;
        for (uint64_t value = n; value; value >>= 1) ++k;
        uint64_t r = getrandbits(k);
        while (r >= n) r = getrandbits(k);
        return r;
    }

    int64_t randrange(int64_t start, int64_t stop) { return start + static_cast<int64_t>(randbelow(uint64_t(stop - start))); }
    int64_t randrange(int64_t stop) { return static_cast<int64_t>(randbelow(uint64_t(stop))); }
    int64_t randint(int64_t low, int64_t high) { return randrange(low, high + 1); }

private:
    static constexpr size_t kN = 624, kM = 397;

    void init_genrand(uint32_t seed) {
        state_[0] = seed;
        for (size_t i = 1; i < kN; ++i) {
            state_[i] = 1812433253u * (state_[i - 1] ^ (state_[i - 1] >> 30)) + static_cast<uint32_t>(i);
        }
        index_ = kN;
    }

    void init_by_array(const uint32_t *key, size_t length) {
        init_genrand(19650218u);
        size_t i = 1, j = 0;
        for (size_t k = kN > length ? kN : length; k; --k) {
            state_[i] = (state_[i] ^ ((state_[i - 1] ^ (state_[i - 1] >> 30)) * 1664525u)) +
                        key[j] + static_cast<uint32_t>(j);
            if (++i >= kN) { state_[0] = state_[kN - 1]; i = 1; }
            if (++j >= length) j = 0;
        }
        for (size_t k = kN - 1; k; --k) {
            state_[i] = (state_[i] ^ ((state_[i - 1] ^ (state_[i - 1] >> 30)) * 1566083941u)) -
                        static_cast<uint32_t>(i);
            if (++i >= kN) { state_[0] = state_[kN - 1]; i = 1; }
        }
        state_[0] = 0x80000000u;
    }

    void generate() {
        const auto mix = [](uint32_t upper, uint32_t lower) {
            uint32_t y = (upper & 0x80000000u) | (lower & 0x7fffffffu);
            return (y >> 1) ^ ((y & 1u) ? 0x9908b0dfu : 0u);
        };
        size_t kk = 0;
        for (; kk < kN - kM; ++kk) state_[kk] = state_[kk + kM] ^ mix(state_[kk], state_[kk + 1]);
        for (; kk < kN - 1; ++kk) state_[kk] = state_[kk + kM - kN] ^ mix(state_[kk], state_[kk + 1]);
        state_[kN - 1] = state_[kM - 1] ^ mix(state_[kN - 1], state_[0]);
        index_ = 0;
    }

    std::array<uint32_t, kN> state_{};
    size_t index_ = kN;
};

} // namespace lh
