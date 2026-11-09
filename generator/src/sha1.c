#include "sha1.h"

#include <string.h>

static uint32_t rotate(uint32_t value, unsigned bits)
{
    return (value << bits) | (value >> (32 - bits));
}

static void transform(uint32_t state[5], const uint8_t block[64])
{
    uint32_t w[80];
    for (int t = 0; t < 16; t++)
        w[t] = (uint32_t)block[t * 4] << 24 | (uint32_t)block[t * 4 + 1] << 16 | (uint32_t)block[t * 4 + 2] << 8 |
               (uint32_t)block[t * 4 + 3];
    for (int t = 16; t < 80; t++)
        w[t] = rotate(w[t - 3] ^ w[t - 8] ^ w[t - 14] ^ w[t - 16], 1);
    uint32_t a = state[0], b = state[1], c = state[2], d = state[3], e = state[4];
    for (int t = 0; t < 80; t++) {
        uint32_t f, k;
        if (t < 20) {
            f = (b & c) | (~b & d);
            k = 0x5A827999;
        } else if (t < 40) {
            f = b ^ c ^ d;
            k = 0x6ED9EBA1;
        } else if (t < 60) {
            f = (b & c) | (b & d) | (c & d);
            k = 0x8F1BBCDC;
        } else {
            f = b ^ c ^ d;
            k = 0xCA62C1D6;
        }
        uint32_t next = rotate(a, 5) + f + e + k + w[t];
        e = d;
        d = c;
        c = rotate(b, 30);
        b = a;
        a = next;
    }
    state[0] += a;
    state[1] += b;
    state[2] += c;
    state[3] += d;
    state[4] += e;
}

void lhg_sha1(const uint8_t *data, size_t size, uint8_t digest[20])
{
    uint32_t state[5] = {0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0};
    size_t remaining = size;
    for (; remaining >= 64; data += 64, remaining -= 64)
        transform(state, data);
    uint8_t block[64];
    memcpy(block, data, remaining);
    block[remaining++] = 0x80;
    if (remaining > 56) {
        memset(block + remaining, 0, 64 - remaining);
        transform(state, block);
        remaining = 0;
    }
    memset(block + remaining, 0, 56 - remaining);
    uint64_t bits = (uint64_t)size * 8;
    for (int index = 0; index < 8; index++)
        block[56 + index] = (uint8_t)(bits >> (56 - index * 8));
    transform(state, block);
    for (int index = 0; index < 20; index++)
        digest[index] = (uint8_t)(state[index / 4] >> (24 - (index % 4) * 8));
}

void lhg_sha1_hex(const uint8_t *data, size_t size, char hex[41])
{
    static const char digits[] = "0123456789abcdef";
    uint8_t digest[20];
    lhg_sha1(data, size, digest);
    for (int index = 0; index < 20; index++) {
        hex[index * 2] = digits[digest[index] >> 4];
        hex[index * 2 + 1] = digits[digest[index] & 15];
    }
    hex[40] = 0;
}

uint32_t lhg_fnv1a(const uint8_t *data, size_t size)
{
    uint32_t value = 2166136261u;
    for (size_t index = 0; index < size; index++)
        value = (value ^ data[index]) * 16777619u;
    return value;
}

uint64_t lhg_fnv64_add(uint64_t state, const uint8_t *data, size_t size)
{
    for (size_t index = 0; index < size; index++)
        state = (state ^ data[index]) * UINT64_C(1099511628211);
    return state;
}
