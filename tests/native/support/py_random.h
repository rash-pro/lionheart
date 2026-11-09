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
#include <cstring>
#include <string>
#include <vector>

namespace lh {

class PyRandom {
public:
    explicit PyRandom(uint64_t seed) {
        std::array<uint32_t, 2> key = {static_cast<uint32_t>(seed), static_cast<uint32_t>(seed >> 32)};
        size_t length = key[1] ? 2 : 1;
        init_by_array(key.data(), length);
    }

    // random.Random('text') seeds from the integer whose big-endian bytes are
    // the text followed by its SHA-512, digits little-endian; see random.py.
    explicit PyRandom(const std::string &seed) {
        std::string bytes = seed;
        std::array<uint8_t, 64> digest = sha512(seed);
        bytes.append(reinterpret_cast<const char *>(digest.data()), digest.size());
        std::vector<uint32_t> key((bytes.size() + 3) / 4, 0);
        for (size_t i = 0; i < bytes.size(); ++i) {
            const size_t from_end = bytes.size() - 1 - i;
            key[from_end / 4] |= uint32_t(static_cast<uint8_t>(bytes[i])) << ((from_end % 4) * 8);
        }
        while (key.size() > 1 && key.back() == 0) key.pop_back();
        init_by_array(key.data(), key.size());
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

    // random.choice(sequence) over a sequence the case already has.
    template <class Sequence> auto choice(const Sequence &values) -> decltype(values[0]) {
        return values[static_cast<size_t>(randbelow(values.size()))];
    }

private:
    static constexpr size_t kN = 624, kM = 397;

    // SHA-512, FIPS 180-4. Only string seeds use it.
    static std::array<uint8_t, 64> sha512(const std::string &text) {
        static const uint64_t k[80] = {
            0x428a2f98d728ae22,0x7137449123ef65cd,0xb5c0fbcfec4d3b2f,0xe9b5dba58189dbbc,0x3956c25bf348b538,
            0x59f111f1b605d019,0x923f82a4af194f9b,0xab1c5ed5da6d8118,0xd807aa98a3030242,0x12835b0145706fbe,
            0x243185be4ee4b28c,0x550c7dc3d5ffb4e2,0x72be5d74f27b896f,0x80deb1fe3b1696b1,0x9bdc06a725c71235,
            0xc19bf174cf692694,0xe49b69c19ef14ad2,0xefbe4786384f25e3,0x0fc19dc68b8cd5b5,0x240ca1cc77ac9c65,
            0x2de92c6f592b0275,0x4a7484aa6ea6e483,0x5cb0a9dcbd41fbd4,0x76f988da831153b5,0x983e5152ee66dfab,
            0xa831c66d2db43210,0xb00327c898fb213f,0xbf597fc7beef0ee4,0xc6e00bf33da88fc2,0xd5a79147930aa725,
            0x06ca6351e003826f,0x142929670a0e6e70,0x27b70a8546d22ffc,0x2e1b21385c26c926,0x4d2c6dfc5ac42aed,
            0x53380d139d95b3df,0x650a73548baf63de,0x766a0abb3c77b2a8,0x81c2c92e47edaee6,0x92722c851482353b,
            0xa2bfe8a14cf10364,0xa81a664bbc423001,0xc24b8b70d0f89791,0xc76c51a30654be30,0xd192e819d6ef5218,
            0xd69906245565a910,0xf40e35855771202a,0x106aa07032bbd1b8,0x19a4c116b8d2d0c8,0x1e376c085141ab53,
            0x2748774cdf8eeb99,0x34b0bcb5e19b48a8,0x391c0cb3c5c95a63,0x4ed8aa4ae3418acb,0x5b9cca4f7763e373,
            0x682e6ff3d6b2b8a3,0x748f82ee5defb2fc,0x78a5636f43172f60,0x84c87814a1f0ab72,0x8cc702081a6439ec,
            0x90befffa23631e28,0xa4506cebde82bde9,0xbef9a3f7b2c67915,0xc67178f2e372532b,0xca273eceea26619c,
            0xd186b8c721c0c207,0xeada7dd6cde0eb1e,0xf57d4f7fee6ed178,0x06f067aa72176fba,0x0a637dc5a2c898a6,
            0x113f9804bef90dae,0x1b710b35131c471b,0x28db77f523047d84,0x32caab7b40c72493,0x3c9ebe0a15c9bebc,
            0x431d67c49c100d4c,0x4cc5d4becb3e42b6,0x597f299cfc657e2a,0x5fcb6fab3ad6faec,0x6c44198c4a475817};
        uint64_t h[8] = {0x6a09e667f3bcc908,0xbb67ae8584caa73b,0x3c6ef372fe94f82b,0xa54ff53a5f1d36f1,
                         0x510e527fade682d1,0x9b05688c2b3e6c1f,0x1f83d9abfb41bd6b,0x5be0cd19137e2179};
        const auto rotr = [](uint64_t x, unsigned n) { return (x >> n) | (x << (64 - n)); };
        std::string message = text;
        message.push_back(static_cast<char>(0x80));
        while (message.size() % 128 != 112) message.push_back('\0');
        for (int shift = 56; shift >= 0; shift -= 8) message.push_back('\0');   // upper 64 length bits
        const uint64_t bits = uint64_t(text.size()) * 8;
        for (int shift = 56; shift >= 0; shift -= 8) message.push_back(static_cast<char>(bits >> shift));
        for (size_t offset = 0; offset < message.size(); offset += 128) {
            uint64_t w[80];
            for (int t = 0; t < 16; ++t) {
                w[t] = 0;
                for (int b = 0; b < 8; ++b) {
                    w[t] = (w[t] << 8) | static_cast<uint8_t>(message[offset + t * 8 + b]);
                }
            }
            for (int t = 16; t < 80; ++t) {
                const uint64_t s0 = rotr(w[t - 15], 1) ^ rotr(w[t - 15], 8) ^ (w[t - 15] >> 7);
                const uint64_t s1 = rotr(w[t - 2], 19) ^ rotr(w[t - 2], 61) ^ (w[t - 2] >> 6);
                w[t] = w[t - 16] + s0 + w[t - 7] + s1;
            }
            uint64_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
            for (int t = 0; t < 80; ++t) {
                const uint64_t S1 = rotr(e, 14) ^ rotr(e, 18) ^ rotr(e, 41);
                const uint64_t temp1 = hh + S1 + ((e & f) ^ (~e & g)) + k[t] + w[t];
                const uint64_t S0 = rotr(a, 28) ^ rotr(a, 34) ^ rotr(a, 39);
                const uint64_t temp2 = S0 + ((a & b) ^ (a & c) ^ (b & c));
                hh = g; g = f; f = e; e = d + temp1; d = c; c = b; b = a; a = temp1 + temp2;
            }
            h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
        }
        std::array<uint8_t, 64> digest;
        for (int i = 0; i < 8; ++i) {
            for (int b = 0; b < 8; ++b) digest[i * 8 + b] = static_cast<uint8_t>(h[i] >> (56 - b * 8));
        }
        return digest;
    }

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
