#include "gte.h"
#if defined(FF8_PGXP)
#include "pgxp.h"
#endif
#include "gte_profile.h"


/* Integer GTE, from the hardware equations and register descriptions in
 * https://psx-spx.consoledev.net/geometrytransformationenginegte/ .
 * Accumulators wrap at 44 bits between matrix additions, then at 32 bits
 * after sf. Keep those boundaries separate from the saturating IR/FIFO paths.
 */
static int32_t signed16(uint32_t n) {
    return (int32_t)(n & 0x7fffu) - (int32_t)(n & 0x8000u);
}

static int32_t signed32(uint32_t n) {
    return n <= 0x7fffffffu ? (int32_t)n : -1 - (int32_t)~n;
}

/* C99 does not specify right shift of negative signed integers. */
static int64_t asr(int64_t n, unsigned bits) {
#ifdef FF8_GTE_REFERENCE
    return n >= 0 ? n / (INT64_C(1) << bits)
                  : -1 - ((-1 - n) / (INT64_C(1) << bits));
#else
    /* Unsigned shifts are defined by C99. The negative branch shifts the
     * complemented magnitude, including INT64_MIN, then restores floor. */
    return n >= 0 ? (int64_t)((uint64_t)n >> bits)
                  : -1 - (int64_t)((uint64_t)(-1 - n) >> bits);
#endif
}

static void flag(cpu_t *c, unsigned bit) {
    c->gte_ctrl[31] |= UINT32_C(1) << bit;
}

static uint32_t summary(uint32_t flags) {
    flags &= 0x7ffff000u;
    return flags | ((flags & 0x7f87e000u) ? 0x80000000u : 0);
}

static int64_t accum(cpu_t *c, unsigned axis, int64_t n) {
    if (n > INT64_C(0x7ffffffffff)) flag(c, 31 - axis);
    if (n < -INT64_C(0x80000000000)) flag(c, 28 - axis);
    uint64_t bits = (uint64_t)n & UINT64_C(0xfffffffffff);
    return bits & UINT64_C(0x80000000000)
        ? -1 - (int64_t)(~bits & UINT64_C(0xfffffffffff)) : (int64_t)bits;
}

static int32_t clamp(cpu_t *c, int64_t n, int32_t low, int32_t high, unsigned bit) {
    if (n < low) { flag(c, bit); return low; }
    if (n > high) { flag(c, bit); return high; }
    return (int32_t)n;
}

static void mac0_check(cpu_t *c, int64_t n) {
    if (n > INT64_C(0x7fffffff)) flag(c, 16);
    if (n < -INT64_C(0x80000000)) flag(c, 15);
}

static void mac0(cpu_t *c, int64_t n) {
    mac0_check(c, n);
    c->gte_data[24] = (uint32_t)n;
}

#if defined(FF8_PLAY) && !defined(FF8_GTE_REFERENCE)
__attribute__((always_inline))
static inline
#else
static
#endif
void result(cpu_t *c, unsigned axis, int64_t n, unsigned shift, int lm) {
    int32_t m = signed32((uint32_t)asr(accum(c, axis, n), shift));
    c->gte_data[24 + axis] = (uint32_t)m;
    c->gte_data[8 + axis] = (uint32_t)clamp(c, m, lm ? 0 : -32768, 32767, 25 - axis);
}

#if defined(FF8_PLAY) && !defined(FF8_GTE_REFERENCE)
__attribute__((always_inline))
static inline void result_prepared(cpu_t *c, unsigned axis, int64_t n, unsigned shift, int lm) {
    /* multiply_prepared has already checked/wrapped every prefix, or proved
     * the whole sum inside signed44. Publication needs no second wrap/check. */
    int32_t m = signed32((uint32_t)asr(n, shift));
    c->gte_data[24 + axis] = (uint32_t)m;
    c->gte_data[8 + axis] = (uint32_t)clamp(c, m, lm ? 0 : -32768, 32767, 25 - axis);
}
#endif

static void vector(const cpu_t *c, unsigned select, int32_t v[3]) {
    unsigned i;
    if (select == 3) {
        for (i = 0; i < 3; ++i) v[i] = signed16(c->gte_data[9 + i]);
    } else {
        v[0] = signed16(c->gte_data[select * 2]);
        v[1] = signed16(c->gte_data[select * 2] >> 16);
        v[2] = signed16(c->gte_data[select * 2 + 1]);
    }
}

typedef struct { int32_t values[3][3]; } gte_matrix_t;
static void matrix(const cpu_t *c, unsigned select, gte_matrix_t *out) {
    int32_t (*m)[3] = out->values;
    unsigned i;
    if (select == 3) {
        m[0][0] = -(int32_t)(c->gte_data[6] & 255) * 16;
        m[0][1] = -m[0][0];
        m[0][2] = signed16(c->gte_data[8]);
        for (i = 0; i < 3; ++i) {
            m[1][i] = signed16(c->gte_ctrl[1]);
            m[2][i] = signed16(c->gte_ctrl[2]);
        }
    } else {
        for (i = 0; i < 9; ++i)
            m[i / 3][i % 3] = signed16(c->gte_ctrl[select * 8 + i / 2] >> ((i % 2) * 16));
    }
}

#if defined(FF8_PLAY) && !defined(FF8_GTE_REFERENCE)
__attribute__((always_inline)) static inline
#else
static
#endif
void multiply_prepared(cpu_t *c, const gte_matrix_t *m, unsigned vs, unsigned cv,
                              unsigned shift, int lm, int64_t raw[3]) {
    int32_t v[3];
    unsigned i;
    vector(c, vs, v);
    for (i = 0; i < 3; ++i) {
        int64_t t = cv == 3 ? 0 : (int64_t)signed32(c->gte_ctrl[5 + cv * 8 + i]) * 4096;
        int64_t n;
#if defined(FF8_PLAY) && !defined(FF8_GTE_REFERENCE)
        /* Three signed16 products have magnitude at most 3 * 2^30.
         * This translation range leaves 2^32 of room at both 44-bit limits,
         * so no prefix can wrap or set an accumulator-overflow flag. Keep
         * far-color's discarded stage and all boundary cases on the oracle. */
        int32_t translation = cv == 3 ? 0 : signed32(c->gte_ctrl[5 + cv * 8 + i]);
        if (cv != 2 && (uint32_t)translation + 0x7ff00000u <= 0xffe00000u) {
            n = t + (int64_t)m->values[i][0] * v[0]
                  + (int64_t)m->values[i][1] * v[1]
                  + (int64_t)m->values[i][2] * v[2];
        } else
#endif
        {
        n = accum(c, i + 1, t + (int64_t)m->values[i][0] * v[0]);
        if (cv == 2) {
            /* Far-color MVMVA discards the translation/first-column result,
             * but still reports its overflow and signed IR saturation. */
            clamp(c, signed32((uint32_t)asr(n, shift)), -32768, 32767, 24 - i);
            n = 0;
        }
        n = accum(c, i + 1, n + (int64_t)m->values[i][1] * v[1]);
        n = accum(c, i + 1, n + (int64_t)m->values[i][2] * v[2]);
        }
        if (raw) raw[i] = n;
#if defined(FF8_PLAY) && !defined(FF8_GTE_REFERENCE)
        else result_prepared(c, i + 1, n, shift, lm);
#else
        else result(c, i + 1, n, shift, lm);
#endif
    }
}

static void multiply(cpu_t *c, unsigned mx, unsigned vs, unsigned cv,
                     unsigned shift, int lm, int64_t raw[3]) {
    gte_matrix_t m;
    matrix(c, mx, &m);
    multiply_prepared(c, &m, vs, cv, shift, lm, raw);
}

static void push_xy(cpu_t *c, uint32_t xy) {
    c->gte_data[12] = c->gte_data[13];
    c->gte_data[13] = c->gte_data[14];
    c->gte_data[14] = xy;
}

static void push_rgb(cpu_t *c) {
    uint32_t rgb = c->gte_data[6] & 0xff000000u;
    unsigned i;
    for (i = 0; i < 3; ++i)
        rgb |= (uint32_t)clamp(c, asr(signed32(c->gte_data[25 + i]), 4), 0, 255, 21 - i) << (i * 8);
    c->gte_data[20] = c->gte_data[21];
    c->gte_data[21] = c->gte_data[22];
    c->gte_data[22] = rgb;
}

/* UNR reciprocal seeds: max(0, ((0x40000/(i+0x100)+1)/2)-0x101),
 * including entry 0x100. These are hardware constants, not emulator code. */
static const uint8_t unr[257] = {
    0xff,0xfd,0xfb,0xf9,0xf7,0xf5,0xf3,0xf1,0xef,0xee,0xec,0xea,0xe8,0xe6,0xe4,0xe3,
    0xe1,0xdf,0xdd,0xdc,0xda,0xd8,0xd6,0xd5,0xd3,0xd1,0xd0,0xce,0xcd,0xcb,0xc9,0xc8,
    0xc6,0xc5,0xc3,0xc1,0xc0,0xbe,0xbd,0xbb,0xba,0xb8,0xb7,0xb5,0xb4,0xb2,0xb1,0xb0,
    0xae,0xad,0xab,0xaa,0xa9,0xa7,0xa6,0xa4,0xa3,0xa2,0xa0,0x9f,0x9e,0x9c,0x9b,0x9a,
    0x99,0x97,0x96,0x95,0x94,0x92,0x91,0x90,0x8f,0x8d,0x8c,0x8b,0x8a,0x89,0x87,0x86,
    0x85,0x84,0x83,0x82,0x81,0x7f,0x7e,0x7d,0x7c,0x7b,0x7a,0x79,0x78,0x77,0x75,0x74,
    0x73,0x72,0x71,0x70,0x6f,0x6e,0x6d,0x6c,0x6b,0x6a,0x69,0x68,0x67,0x66,0x65,0x64,
    0x63,0x62,0x61,0x60,0x5f,0x5e,0x5d,0x5d,0x5c,0x5b,0x5a,0x59,0x58,0x57,0x56,0x55,
    0x54,0x53,0x53,0x52,0x51,0x50,0x4f,0x4e,0x4d,0x4d,0x4c,0x4b,0x4a,0x49,0x48,0x48,
    0x47,0x46,0x45,0x44,0x43,0x43,0x42,0x41,0x40,0x3f,0x3f,0x3e,0x3d,0x3c,0x3c,0x3b,
    0x3a,0x39,0x39,0x38,0x37,0x36,0x36,0x35,0x34,0x33,0x33,0x32,0x31,0x31,0x30,0x2f,
    0x2e,0x2e,0x2d,0x2c,0x2c,0x2b,0x2a,0x2a,0x29,0x28,0x28,0x27,0x26,0x26,0x25,0x24,
    0x24,0x23,0x22,0x22,0x21,0x20,0x20,0x1f,0x1e,0x1e,0x1d,0x1d,0x1c,0x1b,0x1b,0x1a,
    0x19,0x19,0x18,0x18,0x17,0x16,0x16,0x15,0x15,0x14,0x14,0x13,0x12,0x12,0x11,0x11,
    0x10,0x0f,0x0f,0x0e,0x0e,0x0d,0x0d,0x0c,0x0c,0x0b,0x0a,0x0a,0x09,0x09,0x08,0x08,
    0x07,0x07,0x06,0x06,0x05,0x05,0x04,0x04,0x03,0x03,0x02,0x02,0x01,0x01,0x00,0x00,
    0x00
};

static uint32_t divide(cpu_t *c, uint32_t h, uint32_t z) {
    unsigned shift = 0;
    uint32_t d, u, reciprocal;
    uint64_t quotient;
    if (h >= z * 2) { flag(c, 17); return 0x1ffff; }
    while (!(z & 0x8000u)) { z <<= 1; ++shift; }
    d = z;
    u = unr[(d - 0x7fc0u) >> 7] + 0x101u;
    reciprocal = (0x2000080u - d * u) >> 8;
    reciprocal = (0x80u + reciprocal * u) >> 8;
    quotient = ((uint64_t)(h << shift) * reciprocal + 0x8000u) >> 16;
    return quotient > 0x1ffffu ? 0x1ffffu : (uint32_t)quotient;
}

#if defined(FF8_PGXP)
/* Both the generic oracle and specialized exact/play commands publish here. */
static void project_xy(cpu_t *c, uint32_t xy, int64_t x, int64_t y, uint32_t depth) {
    push_xy(c, xy);
    pgxp_record(c, xy, x, y, depth);
}
#else
#define project_xy(c, xy, x, y, depth) push_xy(c, xy)
#endif

static void project(cpu_t *c, unsigned v, unsigned shift, int lm, int last,
                    const gte_matrix_t *prepared) {
    int64_t raw[3], depth, x, y;
    int32_t z_ir, sx, sy;
    uint32_t q;
    if (prepared) multiply_prepared(c, prepared, v, 0, shift, lm, raw);
    else multiply(c, 0, v, 0, shift, lm, raw);
#if defined(FF8_PLAY) && !defined(FF8_GTE_REFERENCE)
    result_prepared(c, 1, raw[0], shift, lm);
    result_prepared(c, 2, raw[1], shift, lm);
#else
    result(c, 1, raw[0], shift, lm);
    result(c, 2, raw[1], shift, lm);
#endif
    c->gte_data[27] = (uint32_t)asr(raw[2], shift);
    depth = asr(raw[2], 12);
    /* RTPS tests IR3 overflow at a fixed 12-bit shift, with signed limits.
     * Stored IR3 instead clamps the 32-bit MAC3 using the requested lm. */
    clamp(c, depth, -32768, 32767, 22);
    z_ir = signed32(c->gte_data[27]);
    if (z_ir < (lm ? 0 : -32768)) z_ir = lm ? 0 : -32768;
    if (z_ir > 32767) z_ir = 32767;
    c->gte_data[11] = (uint32_t)z_ir;
    c->gte_data[16] = c->gte_data[17];
    c->gte_data[17] = c->gte_data[18];
    c->gte_data[18] = c->gte_data[19];
    c->gte_data[19] = (uint32_t)clamp(c, depth, 0, 65535, 18);
    q = divide(c, c->gte_ctrl[26] & 65535u, c->gte_data[19]);
    x = (int64_t)q * signed16(c->gte_data[9]) + signed32(c->gte_ctrl[24]);
    y = (int64_t)q * signed16(c->gte_data[10]) + signed32(c->gte_ctrl[25]);
    mac0_check(c, x);
    mac0_check(c, y);
    sx = clamp(c, asr(x, 16), -1024, 1023, 14);
    sy = clamp(c, asr(y, 16), -1024, 1023, 13);
    project_xy(c, ((uint32_t)sx & 65535u) | ((uint32_t)sy << 16), x, y, (uint32_t)depth);
    if (last) {
        int64_t n = (int64_t)q * signed16(c->gte_ctrl[27]) + signed32(c->gte_ctrl[28]);
        mac0(c, n);
        c->gte_data[8] = (uint32_t)clamp(c, asr(n, 12), 0, 4096, 12);
    }
}

static void interpolate(cpu_t *c, const int64_t base[3], unsigned shift, int lm) {
    unsigned i;
    int32_t ir0 = signed16(c->gte_data[8]);
    for (i = 0; i < 3; ++i) {
        int64_t delta = accum(c, i + 1, (int64_t)signed32(c->gte_ctrl[21 + i]) * 4096 - base[i]);
        int32_t ir = clamp(c, signed32((uint32_t)asr(delta, shift)), -32768, 32767, 24 - i);
        result(c, i + 1, base[i] + (int64_t)ir * ir0, shift, lm);
    }
}

static void modulate(cpu_t *c, unsigned shift, int lm, int cue) {
    int64_t base[3];
    unsigned i;
    for (i = 0; i < 3; ++i)
        base[i] = (int64_t)((c->gte_data[6] >> (i * 8)) & 255) * signed16(c->gte_data[9 + i]) * 16;
    if (cue) interpolate(c, base, shift, lm);
    else for (i = 0; i < 3; ++i) result(c, i + 1, base[i], shift, lm);
}

static void normal_color(cpu_t *c, unsigned v, unsigned shift, int lm, int mode,
                         const gte_matrix_t *light, const gte_matrix_t *color) {
    if (light) multiply_prepared(c, light, v, 3, shift, lm, NULL);
    else multiply(c, 1, v, 3, shift, lm, NULL);
    if (color) multiply_prepared(c, color, 3, 1, shift, lm, NULL);
    else multiply(c, 2, 3, 1, shift, lm, NULL);
    if (mode) modulate(c, shift, lm, mode == 2);
    push_rgb(c);
}

uint32_t gte_read(cpu_t *c, unsigned reg, int control) {
    GTE_PROFILE_READ(c);
    if (control) return c->gte_ctrl[reg];
    if (reg == 15) return c->gte_data[14];
    if (reg == 28 || reg == 29) {
        uint32_t rgb = 0;
        unsigned i;
        for (i = 0; i < 3; ++i) {
            int32_t ir = signed16(c->gte_data[9 + i]);
            uint32_t component = ir < 0 ? 0 : ir > 3968 ? 31 : (uint32_t)ir >> 7;
            rgb |= component << (i * 5);
        }
        return rgb;
    }
    return c->gte_data[reg];
}

void gte_write(cpu_t *c, unsigned reg, uint32_t value, int control) {
    GTE_PROFILE_WRITE(c);
    if (control) {
        if (reg == 4 || reg == 12 || reg == 20 || reg == 26 || reg == 27 || reg == 29 || reg == 30)
            value = (uint32_t)signed16(value);
        if (reg == 31) value = summary(value);
        c->gte_ctrl[reg] = value;
        return;
    }
    if (reg == 29 || reg == 31) return;
    if (reg == 1 || reg == 3 || reg == 5 || (reg >= 8 && reg <= 11))
        value = (uint32_t)signed16(value);
    if (reg == 7 || (reg >= 16 && reg <= 19)) value &= 65535u;
    if (reg == 15) push_xy(c, value);
    if (reg == 28) {
        unsigned i;
        for (i = 0; i < 3; ++i) c->gte_data[9 + i] = ((value >> (i * 5)) & 31) << 7;
        value &= 32767u;
    }
    if (reg == 30) {
        uint32_t bits = value & 0x80000000u ? ~value : value;
        unsigned count = 0;
        while (count < 32 && !(bits & 0x80000000u)) { ++count; bits <<= 1; }
        c->gte_data[31] = count;
    }
    c->gte_data[reg] = value;
}

#ifdef FF8_GTE_REFERENCE
/* Keep the generic oracle independent of the specialized dispatch. */
void gte_cmd(cpu_t *c, uint32_t opcode) {
    unsigned shift = (opcode & (1u << 19)) ? 12 : 0;
    int lm = (opcode & (1u << 10)) != 0;
    unsigned cmd = opcode & 63, i, count;
    int32_t v[3];
    int64_t base[3];
    gte_profile_command(c, cmd);
    ++c->gte_commands;
    c->gte_ctrl[31] = 0;
    switch (cmd) {
    case 0x01: project(c, 0, shift, lm, 1, NULL); break; /* RTPS */
    case 0x30: /* RTPT: only the third vertex writes MAC0/IR0. */
    {
        for (i = 0; i < 3; ++i) project(c, i, shift, lm, i == 2, NULL);
        break;
    }
    case 0x06: { /* NCLIP */
        int32_t x[3], y[3];
        for (i = 0; i < 3; ++i) {
            x[i] = signed16(c->gte_data[12 + i]);
            y[i] = signed16(c->gte_data[12 + i] >> 16);
        }
        mac0(c, (int64_t)x[0] * (y[1] - y[2]) + (int64_t)x[1] * (y[2] - y[0]) +
                (int64_t)x[2] * (y[0] - y[1]));
        break;
    }
    case 0x0c: { /* OP */
        int32_t d[3];
        vector(c, 3, v);
        for (i = 0; i < 3; ++i) d[i] = signed16(c->gte_ctrl[i * 2]);
        for (i = 0; i < 3; ++i) {
            unsigned a = (i + 1) % 3, b = (i + 2) % 3;
            result(c, i + 1, (int64_t)v[b] * d[a] - (int64_t)v[a] * d[b], shift, lm);
        }
        break;
    }
    case 0x12: /* MVMVA */
        multiply(c, (opcode >> 17) & 3, (opcode >> 15) & 3, (opcode >> 13) & 3, shift, lm, NULL);
        break;
    case 0x28: /* SQR */
        vector(c, 3, v);
        for (i = 0; i < 3; ++i) result(c, i + 1, (int64_t)v[i] * v[i], shift, lm);
        break;
    case 0x2d: case 0x2e: { /* AVSZ3, AVSZ4 */
        int64_t sum = 0, n;
        for (i = cmd == 0x2d ? 17 : 16; i <= 19; ++i) sum += c->gte_data[i] & 65535u;
        n = sum * signed16(c->gte_ctrl[cmd == 0x2d ? 29 : 30]);
        mac0(c, n);
        c->gte_data[7] = (uint32_t)clamp(c, asr(n, 12), 0, 65535, 18);
        break;
    }
    case 0x10: case 0x2a: /* DPCS, DPCT */
        count = cmd == 0x2a ? 3 : 1;
        while (count--) {
            uint32_t rgb = c->gte_data[cmd == 0x2a ? 20 : 6];
            for (i = 0; i < 3; ++i) base[i] = (int64_t)((rgb >> (i * 8)) & 255) * 65536;
            interpolate(c, base, shift, lm);
            push_rgb(c);
        }
        break;
    case 0x11: /* INTPL */
        for (i = 0; i < 3; ++i) base[i] = (int64_t)signed16(c->gte_data[9 + i]) * 4096;
        interpolate(c, base, shift, lm);
        push_rgb(c);
        break;
    case 0x29: /* DCPL */
        modulate(c, shift, lm, 1);
        push_rgb(c);
        break;
    case 0x14: case 0x1c: /* CDP, CC */
        multiply(c, 2, 3, 1, shift, lm, NULL);
        modulate(c, shift, lm, cmd == 0x14);
        push_rgb(c);
        break;
    case 0x13: case 0x16: /* NCDS, NCDT */
    case 0x1b: case 0x3f: /* NCCS, NCCT */
    case 0x1e: case 0x20: /* NCS, NCT */
    {
        count = cmd == 0x16 || cmd == 0x3f || cmd == 0x20 ? 3 : 1;
        for (i = 0; i < count; ++i)
            normal_color(c, i, shift, lm, cmd == 0x13 || cmd == 0x16 ? 2 :
                                        cmd == 0x1b || cmd == 0x3f ? 1 : 0, NULL, NULL);
        break;
    }
    case 0x3d: case 0x3e: /* GPF, GPL */
        vector(c, 3, v);
        for (i = 0; i < 3; ++i) {
            int64_t n = (int64_t)v[i] * signed16(c->gte_data[8]);
            if (cmd == 0x3e) n += (int64_t)signed32(c->gte_data[25 + i]) * (INT64_C(1) << shift);
            result(c, i + 1, n, shift, lm);
        }
        push_rgb(c);
        break;
    default: break; /* Undocumented opcodes have no specified arithmetic. */
    }
    c->gte_ctrl[31] = summary(c->gte_ctrl[31]);
}
#else
/* Native pages import these entry points. Keep a call from gte_cmd so the
 * main ELF's --gc-sections cannot discard an API used only by a page. */
#define GTE_ENTRY

static void command_begin(cpu_t *c, unsigned opcode) {
    gte_profile_command(c, opcode);
    (void)opcode;
    ++c->gte_commands;
    c->gte_ctrl[31] = 0;
}

static void command_end(cpu_t *c) {
    c->gte_ctrl[31] = summary(c->gte_ctrl[31]);
}

static void normal_colors(cpu_t *c, unsigned shift, int lm, unsigned count, int mode) {
    unsigned i;
    for (i = 0; i < count; ++i) normal_color(c, i, shift, lm, mode, NULL, NULL);
}

void GTE_ENTRY gte_rtps(cpu_t *c, unsigned shift, int lm) {
    command_begin(c, 0x01);
    project(c, 0, shift, lm, 1, NULL);
    command_end(c);
}

void GTE_ENTRY gte_nclip(cpu_t *c) {
    command_begin(c, 0x06);
    int32_t x[3], y[3];
    unsigned i;
    for (i = 0; i < 3; ++i) {
        x[i] = signed16(c->gte_data[12 + i]);
        y[i] = signed16(c->gte_data[12 + i] >> 16);
    }
    /* Each signed16 * signed17 product fits in signed32; sum in signed64. */
    mac0(c, (int64_t)(x[0] * (y[1] - y[2])) + (int64_t)(x[1] * (y[2] - y[0])) +
            (int64_t)(x[2] * (y[0] - y[1])));
    command_end(c);
}

void GTE_ENTRY gte_op(cpu_t *c, unsigned shift, int lm) {
    command_begin(c, 0x0c);
    int32_t d[3], v[3];
    unsigned i;
    vector(c, 3, v);
    for (i = 0; i < 3; ++i) d[i] = signed16(c->gte_ctrl[i * 2]);
    for (i = 0; i < 3; ++i) {
        unsigned a = (i + 1) % 3, b = (i + 2) % 3;
        result(c, i + 1, (int64_t)v[b] * d[a] - (int64_t)v[a] * d[b], shift, lm);
    }
    command_end(c);
}

void GTE_ENTRY gte_dpcs(cpu_t *c, unsigned shift, int lm) {
    command_begin(c, 0x10);
    unsigned count = 1, i;
    int64_t base[3];
    while (count--) {
        uint32_t rgb = c->gte_data[6];
        for (i = 0; i < 3; ++i) base[i] = (int64_t)((rgb >> (i * 8)) & 255) * 65536;
        interpolate(c, base, shift, lm);
        push_rgb(c);
    }
    command_end(c);
}

void GTE_ENTRY gte_intpl(cpu_t *c, unsigned shift, int lm) {
    command_begin(c, 0x11);
    int64_t base[3];
    unsigned i;
    for (i = 0; i < 3; ++i) base[i] = (int64_t)signed16(c->gte_data[9 + i]) * 4096;
    interpolate(c, base, shift, lm);
    push_rgb(c);
    command_end(c);
}

void GTE_ENTRY gte_mvmva(cpu_t *c, unsigned shift, int lm, unsigned mx, unsigned vs, unsigned cv) {
    command_begin(c, 0x12);
#ifdef FF8_PLAY
    /* World transforms commonly use rotation/V0/translation with sf=12.
     * Expose these constants without changing the generated command ABI. */
    if (shift == 12 && !lm && !(mx | vs | cv)) {
        gte_matrix_t m;
        matrix(c, 0, &m);
        multiply_prepared(c, &m, 0, 0, 12, 0, NULL);
    } else
#endif
    multiply(c, mx, vs, cv, shift, lm, NULL);
    command_end(c);
}

void GTE_ENTRY gte_ncds(cpu_t *c, unsigned shift, int lm) {
    command_begin(c, 0x13);
    normal_colors(c, shift, lm, 1, 2);
    command_end(c);
}

void GTE_ENTRY gte_cdp(cpu_t *c, unsigned shift, int lm) {
    command_begin(c, 0x14);
    multiply(c, 2, 3, 1, shift, lm, NULL);
    modulate(c, shift, lm, 1);
    push_rgb(c);
    command_end(c);
}

void GTE_ENTRY gte_ncdt(cpu_t *c, unsigned shift, int lm) {
    command_begin(c, 0x16);
    normal_colors(c, shift, lm, 3, 2);
    command_end(c);
}

void GTE_ENTRY gte_nccs(cpu_t *c, unsigned shift, int lm) {
    command_begin(c, 0x1b);
    normal_colors(c, shift, lm, 1, 1);
    command_end(c);
}

void GTE_ENTRY gte_cc(cpu_t *c, unsigned shift, int lm) {
    command_begin(c, 0x1c);
    multiply(c, 2, 3, 1, shift, lm, NULL);
    modulate(c, shift, lm, 0);
    push_rgb(c);
    command_end(c);
}

void GTE_ENTRY gte_ncs(cpu_t *c, unsigned shift, int lm) {
    command_begin(c, 0x1e);
    normal_colors(c, shift, lm, 1, 0);
    command_end(c);
}

void GTE_ENTRY gte_nct(cpu_t *c, unsigned shift, int lm) {
    command_begin(c, 0x20);
    normal_colors(c, shift, lm, 3, 0);
    command_end(c);
}

void GTE_ENTRY gte_sqr(cpu_t *c, unsigned shift, int lm) {
    command_begin(c, 0x28);
    int32_t v[3];
    unsigned i;
    vector(c, 3, v);
    for (i = 0; i < 3; ++i) result(c, i + 1, (int64_t)v[i] * v[i], shift, lm);
    command_end(c);
}

void GTE_ENTRY gte_dcpl(cpu_t *c, unsigned shift, int lm) {
    command_begin(c, 0x29);
    modulate(c, shift, lm, 1);
    push_rgb(c);
    command_end(c);
}

void GTE_ENTRY gte_dpct(cpu_t *c, unsigned shift, int lm) {
    command_begin(c, 0x2a);
    unsigned count = 3, i;
    int64_t base[3];
    while (count--) {
        uint32_t rgb = c->gte_data[20];
        for (i = 0; i < 3; ++i) base[i] = (int64_t)((rgb >> (i * 8)) & 255) * 65536;
        interpolate(c, base, shift, lm);
        push_rgb(c);
    }
    command_end(c);
}

void GTE_ENTRY gte_avsz3(cpu_t *c) {
    command_begin(c, 0x2d);
    unsigned i;
    int64_t sum = 0;
    int64_t n;
    for (i = 17; i <= 19; ++i) sum += c->gte_data[i] & 65535u;
    n = sum * (int64_t)signed16(c->gte_ctrl[29]);
    mac0(c, n);
    c->gte_data[7] = (uint32_t)clamp(c, asr(n, 12), 0, 65535, 18);
    command_end(c);
}

void GTE_ENTRY gte_avsz4(cpu_t *c) {
    command_begin(c, 0x2e);
    unsigned i;
    int64_t sum = 0;
    int64_t n;
    for (i = 16; i <= 19; ++i) sum += c->gte_data[i] & 65535u;
    n = sum * (int64_t)signed16(c->gte_ctrl[30]);
    mac0(c, n);
    c->gte_data[7] = (uint32_t)clamp(c, asr(n, 12), 0, 65535, 18);
    command_end(c);
}

void GTE_ENTRY gte_rtpt(cpu_t *c, unsigned shift, int lm) {
    command_begin(c, 0x30);
    unsigned i;
    for (i = 0; i < 3; ++i) project(c, i, shift, lm, i == 2, NULL);
    command_end(c);
}

void GTE_ENTRY gte_gpf(cpu_t *c, unsigned shift, int lm) {
    command_begin(c, 0x3d);
    int32_t v[3];
    unsigned i;
    vector(c, 3, v);
    for (i = 0; i < 3; ++i) {
        int64_t n = (int64_t)v[i] * signed16(c->gte_data[8]);
        result(c, i + 1, n, shift, lm);
    }
    push_rgb(c);
    command_end(c);
}

void GTE_ENTRY gte_gpl(cpu_t *c, unsigned shift, int lm) {
    command_begin(c, 0x3e);
    int32_t v[3];
    unsigned i;
    vector(c, 3, v);
    for (i = 0; i < 3; ++i) {
        int64_t n = (int64_t)v[i] * signed16(c->gte_data[8]);
        n += (int64_t)signed32(c->gte_data[25 + i]) * (INT64_C(1) << shift);
        result(c, i + 1, n, shift, lm);
    }
    push_rgb(c);
    command_end(c);
}

void GTE_ENTRY gte_ncct(cpu_t *c, unsigned shift, int lm) {
    command_begin(c, 0x3f);
    normal_colors(c, shift, lm, 3, 1);
    command_end(c);
}

void gte_cmd(cpu_t *c, uint32_t opcode) {
    unsigned shift = (opcode & (1u << 19)) ? 12 : 0;
    int lm = (opcode & (1u << 10)) != 0;
    switch (opcode & 63) {
    case 0x01: gte_rtps(c, shift, lm); break;
    case 0x06: gte_nclip(c); break;
    case 0x0c: gte_op(c, shift, lm); break;
    case 0x10: gte_dpcs(c, shift, lm); break;
    case 0x11: gte_intpl(c, shift, lm); break;
    case 0x12: gte_mvmva(c, shift, lm, (opcode >> 17) & 3, (opcode >> 15) & 3, (opcode >> 13) & 3); break;
    case 0x13: gte_ncds(c, shift, lm); break;
    case 0x14: gte_cdp(c, shift, lm); break;
    case 0x16: gte_ncdt(c, shift, lm); break;
    case 0x1b: gte_nccs(c, shift, lm); break;
    case 0x1c: gte_cc(c, shift, lm); break;
    case 0x1e: gte_ncs(c, shift, lm); break;
    case 0x20: gte_nct(c, shift, lm); break;
    case 0x28: gte_sqr(c, shift, lm); break;
    case 0x29: gte_dcpl(c, shift, lm); break;
    case 0x2a: gte_dpct(c, shift, lm); break;
    case 0x2d: gte_avsz3(c); break;
    case 0x2e: gte_avsz4(c); break;
    case 0x30: gte_rtpt(c, shift, lm); break;
    case 0x3d: gte_gpf(c, shift, lm); break;
    case 0x3e: gte_gpl(c, shift, lm); break;
    case 0x3f: gte_ncct(c, shift, lm); break;
    default:
        command_begin(c, opcode & 63);
        command_end(c);
        break;
    }
}
#undef GTE_ENTRY
#endif
