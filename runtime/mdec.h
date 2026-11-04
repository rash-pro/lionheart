#ifndef FF8_MDEC_H
#define FF8_MDEC_H

#include <stdint.h>

struct mdec;
#ifdef FF8_MDEC_ACCEL
/* A preview may prepare pixels, but only the normal parser consumes input or
 * advances the FIFO/calendar. Every previewed halfword is checked on use. */
typedef struct {
    void (*prepare)(struct mdec *, const uint8_t *ram, uint32_t address, unsigned words, int step);
    void (*cancel)(struct mdec *);
    int (*pixels)(struct mdec *);
} mdec_accel_t;
#endif

typedef struct mdec {
    uint16_t input[64];
    unsigned head, count, remaining, table_pos, opcode;
    struct { uint32_t word; int16_t offset; uint8_t block; } fifo[32];
    unsigned fifo_head, fifo_count;
    uint32_t command, control;
    uint16_t idle_count;
    uint8_t quant[2][64], output[192];
    int16_t scale[64], blocks[3][64];
    unsigned block, coefficient, qscale, output_block, output_pos, output_size;
    uint64_t due, blocks_decoded, words_in, words_out;
#ifdef FF8_MDEC_ACCEL
    const mdec_accel_t *accel;
    const uint16_t *preview;
    unsigned preview_pos, preview_count, deferred_idct;
    const uint16_t *raw_preview;
    unsigned raw_start[3], raw_count[3], raw_blocks;
#endif
} mdec_t;

void mdec_init(mdec_t *);
void mdec_control(mdec_t *, uint32_t);
uint32_t mdec_status(const mdec_t *);
void mdec_write(mdec_t *, uint32_t, uint64_t now);
uint32_t mdec_read(mdec_t *, uint64_t now);
void mdec_event(mdec_t *, uint64_t now);
static inline int mdec_dma_in(const mdec_t *m) {
    return (m->control & 0x40000000u) && m->opcode && m->remaining && !m->count;
}
static inline int mdec_dma_out(const mdec_t *m) { return (m->control & 0x20000000u) && m->fifo_count == 32; }
static inline int mdec_dma_word_ready(const mdec_t *m, unsigned channel) {
    return channel ? ((m->control & 0x20000000u) && m->fifo_count) :
                     ((m->control & 0x40000000u) && m->count <= 62);
}
static inline int32_t mdec_dma_offset(const mdec_t *m) {
    return m->fifo_count ? m->fifo[m->fifo_head].offset : 0;
}

/* Replaceable C99 block boundary for an alternate decoder backend. Coefficients
 * carry four fractional bits; the caller supplies the programmed matrix. */
void mdec_idct(int16_t block[64], const int16_t scale[64]);

#endif
