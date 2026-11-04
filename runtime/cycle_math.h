#ifndef FF8_CYCLE_MATH_H
#define FF8_CYCLE_MATH_H
#include <stdint.h>

/* Explicit limbs keep constant timing ratios exact across uint64_t,
 * without floating point or rounding drift. */
static inline uint64_t cycle_mul(uint64_t a, uint64_t b) {
    return (uint64_t)(uint32_t)a * (uint32_t)b +
        ((uint64_t)((uint32_t)(a >> 32) * (uint32_t)b +
                    (uint32_t)a * (uint32_t)(b >> 32)) << 32);
}
static inline uint64_t cycle_mul_high(uint64_t a, uint64_t b) {
    uint64_t lo = (uint64_t)(uint32_t)a * (uint32_t)b;
    uint64_t x = (uint64_t)(uint32_t)(a >> 32) * (uint32_t)b;
    uint64_t y = (uint64_t)(uint32_t)a * (uint32_t)(b >> 32);
    uint64_t hi = (uint64_t)(uint32_t)(a >> 32) * (uint32_t)(b >> 32);
    uint64_t carry = (lo >> 32) + (uint32_t)x + (uint32_t)y;
    return hi + (x >> 32) + (y >> 32) + (carry >> 32);
}
/* d is a compile-time constant > 1; floor(2^64/d) underestimates the quotient
 * by at most one. One exact remainder comparison corrects it. */
#define CYCLE_DIV(n, d) cycle_div((n), (d), UINT64_MAX / (d))
static inline uint64_t cycle_div(uint64_t n, uint64_t d, uint64_t reciprocal) {
    uint64_t q = cycle_mul_high(n, reciprocal);
    return q + (n - cycle_mul(q, d) >= d);
}
#endif
