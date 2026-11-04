#include "mdec.h"
#include <string.h>

static const uint8_t scan[64] = {
     0, 1, 8,16, 9, 2, 3,10,17,24,32,25,18,11, 4, 5,
    12,19,26,33,40,48,41,34,27,20,13, 6, 7,14,21,28,
    35,42,49,56,57,50,43,36,29,22,15,23,30,37,44,51,
    58,59,52,45,38,31,39,46,53,60,61,54,47,55,62,63
};

/* Explicit floor division avoids implementation-defined signed shifts. */
static int32_t shr(int64_t v, unsigned n) {
    return (int32_t)(v >= 0 ? v / ((int64_t)1 << n) : -((-v + (((int64_t)1 << n) - 1)) / ((int64_t)1 << n)));
}
static int clamp(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }
static int signed_bits(unsigned v, unsigned bits) {
    unsigned mask = (1u << bits) - 1;
    v &= mask;
    return v & (1u << (bits - 1)) ? (int)v - (int)(mask + 1) : (int)v;
}

/* Floor division by 2^n without relying on signed right shifts. */
static int32_t floor_shift(int32_t v, unsigned n) { return v >= 0 ? v >> n : ~(~v >> n); }

void mdec_idct(int16_t block[64], const int16_t scale[64]) {
    int16_t temp[64];
    int32_t matrix[64];
    unsigned pass, row, x;
    /* Two fixed-point matrix passes, including the intermediate rounding.
     * Keep the scale table in upload order (frequency rows, spatial columns).
     * |int16 * (int16 >> 3)| <= 2^27, so an 8-term sum fits int32_t exactly. */
    for (x = 0; x < 64; ++x) matrix[x] = floor_shift(scale[x], 3);
    for (pass = 0; pass < 2; ++pass) {
        for (row = 0; row < 8; ++row) {
            int32_t b0 = block[row], b1 = block[row + 8], b2 = block[row + 16], b3 = block[row + 24];
            int32_t b4 = block[row + 32], b5 = block[row + 40], b6 = block[row + 48], b7 = block[row + 56];
            int16_t *out = temp + row * 8;
            /* A zero input column rounds to zero: floor((0 + 0x4000) / 2^15) = 0. */
            if (!(b0 | b1 | b2 | b3 | b4 | b5 | b6 | b7)) { memset(out, 0, 8 * sizeof(*out)); continue; }
            for (x = 0; x < 8; ++x) {
                const int32_t *m = matrix + x;
                int32_t sum = b0 * m[0] + b1 * m[8] + b2 * m[16] + b3 * m[24] +
                              b4 * m[32] + b5 * m[40] + b6 * m[48] + b7 * m[56];
                out[x] = (int16_t)clamp(floor_shift(sum + 0x4000, 15), -32768, 32767);
            }
        }
        memcpy(block, temp, sizeof(temp));
    }
    for (x = 0; x < 64; ++x) block[x] = (int16_t)clamp(signed_bits((unsigned)block[x], 9), -128, 127);
}

void mdec_init(mdec_t *m) {
    memset(m, 0, sizeof(*m));
    m->block = 4;
    m->coefficient = 64;
    m->due = UINT64_MAX;
}

void mdec_control(mdec_t *m, uint32_t value) {
    if (value & 0x80000000u) {
#ifdef FF8_MDEC_ACCEL
        if (m->accel) m->accel->cancel(m);
        m->preview = m->raw_preview = NULL; m->deferred_idct = m->raw_blocks = 0;
#endif
        /* Reset the pipeline, not the separately programmed tables. */
        m->head = m->count = m->remaining = m->opcode = 0;
        m->fifo_head = m->fifo_count = 0;
        m->output_pos = m->output_size = 0;
        m->command = m->idle_count = 0;
        m->block = 4; m->coefficient = 64; m->due = UINT64_MAX;
    }
    m->control = value & 0x60000000u;
}


uint32_t mdec_status(const mdec_t *m) {
    unsigned block = m->fifo_count ? m->fifo[m->fifo_head].block :
                     m->output_pos < m->output_size ? m->output_block : m->block;
    return (m->fifo_count ? 0 : 0x80000000u) | (m->count == 64 ? 0x40000000u : 0) |
           (m->opcode || m->due != UINT64_MAX || m->output_pos < m->output_size ? 0x20000000u : 0) |
           (mdec_dma_in(m) ? 0x10000000u : 0) |
           (mdec_dma_out(m) ? 0x08000000u : 0) | ((m->command >> 2) & 0x07800000u) |
           (block << 16) | (m->opcode ? ((m->remaining / 2 - 1) & 0xffff) : m->idle_count);
}

static unsigned depth(const mdec_t *m) { return (m->command >> 27) & 3; }
static uint8_t component(const mdec_t *m, int v) {
    int s = clamp(signed_bits((unsigned)v, 9), -128, 127);
    return (uint8_t)(s + (m->command & (1u << 26) ? 0 : 128));
}

static void pixels(mdec_t *m) {
    unsigned i, d = depth(m), b = m->block;
    int16_t *y = m->blocks[2];
    m->output_pos = 0;
    m->output_block = d < 2 ? 4 : b;
    memset(m->output, 0, sizeof(m->output));
    for (i = 0; i < 64; ++i) {
        if (d < 2) {
            unsigned v = component(m, y[i]);
            if (d == 0) m->output[i / 2] |= (v >> 4) << ((i & 1) * 4);
            else m->output[i] = (uint8_t)v;
        } else {
            unsigned uv = ((i % 8) / 2 + (b & 1) * 4) + ((i / 8) / 2 + (b / 2) * 4) * 8;
            int cr = m->blocks[0][uv], cb = m->blocks[1][uv];
            unsigned r = component(m, y[i] + shr(359 * cr + 128, 8));
            unsigned g = component(m, y[i] + shr(shr(-88 * cb, 5) * 32 + shr(-183 * cr, 3) * 8 + 128, 8));
            unsigned blue = component(m, y[i] + shr(454 * cb + 128, 8));
            if (d == 3) {
                /* Hardware rounds/clips unsigned components before applying
                 * the signed-output bias to the packed five-bit values. */
                unsigned bias = m->command & (1u << 26) ? 128 : 0;
                r ^= bias; g ^= bias; blue ^= bias;
                unsigned packed = (unsigned)clamp((r + 4) / 8, 0, 31) |
                    ((unsigned)clamp((g + 4) / 8, 0, 31) << 5) |
                    ((unsigned)clamp((blue + 4) / 8, 0, 31) << 10) |
                    ((m->command & (1u << 25)) ? 0x8000 : 0);
                if (bias) packed ^= 0x4210;
                m->output[i * 2] = (uint8_t)packed; m->output[i * 2 + 1] = (uint8_t)(packed >> 8);
            } else {
                m->output[i * 3] = (uint8_t)r; m->output[i * 3 + 1] = (uint8_t)g; m->output[i * 3 + 2] = (uint8_t)blue;
            }
        }
    }
    m->output_size = d == 0 ? 32 : d == 1 ? 64 : d == 2 ? 192 : 128;
}

static unsigned pop(mdec_t *m) {
    unsigned n = m->input[m->head];
    m->head = (m->head + 1) % 64; --m->count;
    return n;
}

static int16_t coefficient(unsigned n, unsigned k, unsigned qscale, const uint8_t *qt, int first) {
    int value = signed_bits(n, 10), factor = qt[k] * (first ? 1 : qscale), coeff;
    if (!qscale || (!first && !factor)) coeff = value * 32;
    else {
        coeff = first ? value * factor : shr(value * factor, 3);
        coeff = coeff * 16 + (value < 0 ? 8 : value > 0 ? -8 : 0);
    }
    return (int16_t)clamp(coeff, -16384, 16383);
}

#ifdef FF8_MDEC_ACCEL
/* Verified preview words already went through inverse quantization in the
 * encoder. Retain their block ranges and reconstruct coefficients only if the
 * preview is cancelled, a word changes, or the accelerator declines output. */
static void restore_preview(mdec_t *m) {
    unsigned slot;
    for (slot = 0; slot < 3; ++slot) if (m->raw_blocks & (1u << slot)) {
        const uint16_t *raw = m->raw_preview + m->raw_start[slot];
        const uint8_t *qt = m->quant[slot != 2];
        int16_t *block = m->blocks[slot];
        unsigned i, k = 0, qscale = raw[0] >> 10;
        memset(block, 0, sizeof(m->blocks[slot]));
        for (i = 0; i < m->raw_count[slot]; ++i) {
            if (i) k += (raw[i] >> 10) + 1;
            if (k < 64) block[qscale ? scan[k] : k] = coefficient(raw[i], k, qscale, qt, !i);
        }
    }
    m->raw_blocks = 0; m->raw_preview = NULL;
}
#endif

/* Offset of output word p of block b from its linear position, for a fixed
 * words-per-row count (constant at each call, so no division instruction). */
static inline int16_t block_offset(unsigned b, unsigned p, unsigned row_words) {
    return (int16_t)((int)((p / row_words) * row_words) - (int)((b & 1) * 7 * row_words));
}

static int flush_pixels(mdec_t *m) {
    unsigned d = depth(m);
    while (m->output_pos < m->output_size && m->fifo_count < 32) {
        unsigned p = m->output_pos / 4;
        unsigned b = m->output_block, tail = (m->fifo_head + m->fifo_count++) % 32;
        const uint8_t *bytes = m->output + m->output_pos;
        m->fifo[tail].block = (uint8_t)b;
        m->fifo[tail].offset = d < 2 ? 0 : d == 2 ? block_offset(b, p, 6) : block_offset(b, p, 4);
        /* memcpy permits an unaligned word load without violating aliasing. */
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
        memcpy(&m->fifo[tail].word, bytes, 4);
#else
        m->fifo[tail].word = bytes[0] | ((uint32_t)bytes[1] << 8) | ((uint32_t)bytes[2] << 16) | ((uint32_t)bytes[3] << 24);
#endif
        m->output_pos += 4;
    }
    return m->output_pos == m->output_size;
}

#if defined(__GNUC__)
__attribute__((noinline))
#endif
static void decode(mdec_t *m, uint64_t now) {
    while (m->count) {
        unsigned n;
        if (!m->opcode) {
            if (m->count < 2) return;
            m->command = pop(m); m->command |= (uint32_t)pop(m) << 16;
            m->opcode = m->command >> 29;
#ifdef FF8_MDEC_ACCEL
            if (m->accel) m->accel->cancel(m);
            m->preview = m->raw_preview = NULL; m->deferred_idct = m->raw_blocks = 0;
#endif
            m->remaining = m->opcode == 1 ? (m->command & 0xffff) * 2 :
                           m->opcode == 2 ? ((m->command & 1) ? 64 : 32) : m->opcode == 3 ? 64 : 0;
            m->idle_count = (uint16_t)m->command;
            m->table_pos = 0; m->coefficient = 64; m->block = 4;
            if (!m->remaining) {
                if (m->opcode == 1) m->idle_count = 0xffff;
                m->opcode = 0;
            }
            continue;
        }
        n = pop(m); --m->remaining;
#ifdef FF8_MDEC_ACCEL
        if (m->preview && (m->preview_pos >= m->preview_count || n != m->preview[m->preview_pos++])) {
            restore_preview(m); m->preview = NULL;
        }
#endif
        if (m->opcode == 2) {
            unsigned p = m->table_pos++ * 2;
            m->quant[p / 64][p % 64] = (uint8_t)n;
            m->quant[p / 64][p % 64 + 1] = (uint8_t)(n >> 8);
        } else if (m->opcode == 3) {
            m->scale[m->table_pos++] = (int16_t)signed_bits(n, 16);
        } else {
            unsigned mono = depth(m) < 2, slot = mono || m->block < 4 ? 2 : m->block - 4;
            int16_t *blk = m->blocks[slot];
            const uint8_t *qt = m->quant[!mono && m->block >= 4];
            int first = m->coefficient == 64;
            if (first && n == 0xfe00) goto consumed;
            if (first) {
#ifdef FF8_MDEC_ACCEL
                m->deferred_idct &= ~(1u << slot);
                if (m->preview && !mono) {
                    m->raw_preview = m->preview; m->raw_start[slot] = m->preview_pos - 1;
                    m->raw_count[slot] = 0; m->raw_blocks |= 1u << slot;
                } else
#endif
                memset(blk, 0, sizeof(m->blocks[0]));
                m->coefficient = 0; m->qscale = n >> 10;
            } else m->coefficient += (n >> 10) + 1;
#ifdef FF8_MDEC_ACCEL
            if (m->preview && !mono) ++m->raw_count[slot];
            else
#endif
            if (m->coefficient < 64)
                blk[m->qscale ? scan[m->coefficient] : m->coefficient] = coefficient(n, m->coefficient, m->qscale, qt, first);
            if (m->coefficient >= 63) {
#if defined(FF8_PLAY) && defined(FF8_MDEC_ACCEL)
                unsigned completed_block = m->block;
#endif
#ifdef FF8_MDEC_ACCEL
                int accelerated = 0;
                if (m->preview && !mono) {
                    if (m->block >= 4) { m->deferred_idct |= 1u << slot; accelerated = 1; }
                    else accelerated = m->accel->pixels(m);
                }
                if (!accelerated) {
                    unsigned s;
                    restore_preview(m);
                    m->preview = NULL;
                    for (s = 0; s < 2; ++s) if (m->deferred_idct & (1u << s)) mdec_idct(m->blocks[s], m->scale);
                    m->deferred_idct = 0;
                    mdec_idct(blk, m->scale);
                    if (mono || m->block < 4) pixels(m);
                }
#else
                mdec_idct(blk, m->scale);
                if (mono || m->block < 4) pixels(m);
#endif
                ++m->blocks_decoded;
                m->coefficient = 64;
                if (!mono) m->block = (m->block + 1) % 6;
                m->due = now + 512;
#if defined(FF8_PLAY) && defined(FF8_MDEC_ACCEL)
                /* Play may expose luma already supplied by the decoder immediately.
                 * FIFO grants and DMA word dates remain scheduled. */
                if (accelerated && completed_block < 4) m->due = now;
#endif
            }
        }
consumed:
        if (!m->remaining) { m->opcode = 0; m->idle_count = 0xffff; }
        if (m->due != UINT64_MAX) return;
    }
}

void mdec_event(mdec_t *m, uint64_t now) {
#ifdef FF8_MDEC_ACCEL
    if (!m->preview && m->raw_blocks) restore_preview(m);
#endif
    if (m->due != UINT64_MAX) {
        if (m->due > now) return;
        m->due = UINT64_MAX;
    }
    /* FIFO-only steps do not need the parser's coefficient working set or
     * register saves. Keep this frequent calendar path small. */
    if (flush_pixels(m) && m->count) decode(m, now);
}

void mdec_write(mdec_t *m, uint32_t value, uint64_t now) {
    if (m->count > 62) return;
    m->input[(m->head + m->count++) % 64] = (uint16_t)value;
    m->input[(m->head + m->count++) % 64] = (uint16_t)(value >> 16);
    ++m->words_in;
    if (m->due == UINT64_MAX || m->due <= now) mdec_event(m, now);
}

uint32_t mdec_read(mdec_t *m, uint64_t now) {
    uint32_t value;
    /* mdec_event is idempotent, and every state change (write, read, calendar
     * event) already ends with it; reset leaves nothing for it to do. */
    if (!m->fifo_count) return 0xffffffffu;
    value = m->fifo[m->fifo_head].word;
    m->fifo_head = (m->fifo_head + 1) % 32; --m->fifo_count;
    ++m->words_out;
    if (m->due != UINT64_MAX && m->due > now) return value;
    if (m->due == UINT64_MAX && m->output_pos == m->output_size && !m->count) return value;
    mdec_event(m, now);
    return value;
}

