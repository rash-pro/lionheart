#ifndef FF8_SCRATCHPAD_H
#define FF8_SCRATCHPAD_H

#include "cpu.h"

#define SCRATCH_BASE 0x1f800000u
#define SCRATCH_SIZE 1024u

/* Callers check alignment. Isolation retains the timed memory path. */
static inline int c_scratch_fast(const cpu_t *c, uint32_t p, unsigned width) {
    return c->ram_limit && p - SCRATCH_BASE <= SCRATCH_SIZE - width;
}

static inline uint32_t scratch_read(const cpu_t *c, uint32_t p, unsigned width) {
    p -= SCRATCH_BASE;
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
    if (width == 4) return ((const ram_word_t *)c->scratch)[p >> 2];
    if (width == 2) return ((const ram_half_t *)c->scratch)[p >> 1];
    return c->scratch[p];
#else
    uint32_t value = c->scratch[p];
    if (width >= 2) value |= (uint32_t)c->scratch[p + 1] << 8;
    if (width == 4) value |= (uint32_t)c->scratch[p + 2] << 16 | (uint32_t)c->scratch[p + 3] << 24;
    return value;
#endif
}

static inline void scratch_write(cpu_t *c, uint32_t p, unsigned width, uint32_t value) {
    p -= SCRATCH_BASE;
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
    if (width == 4) ((ram_word_t *)c->scratch)[p >> 2] = value;
    else if (width == 2) ((ram_half_t *)c->scratch)[p >> 1] = (uint16_t)value;
    else c->scratch[p] = (uint8_t)value;
#else
    c->scratch[p] = (uint8_t)value;
    if (width >= 2) c->scratch[p + 1] = (uint8_t)(value >> 8);
    if (width == 4) {
        c->scratch[p + 2] = (uint8_t)(value >> 16);
        c->scratch[p + 3] = (uint8_t)(value >> 24);
    }
#endif
}

#endif
