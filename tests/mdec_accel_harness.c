#include "hw.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>

static uint16_t preview[1024];
static unsigned pixel_calls, cancel_calls, complex_input, output_depth = 2;
static void cancel(mdec_t *m) { m->preview = NULL; ++cancel_calls; }
static void prepare(mdec_t *m, const uint8_t *ram, uint32_t address, unsigned words, int step) {
    unsigned i;
    if (m->opcode != 1 || m->remaining != (m->command & 65535) * 2 || m->remaining > 1024 ||
        words < m->remaining / 2 || m->count || m->block != 4 || m->coefficient != 64) return;
    for (i = 0; i < m->remaining; i += 2) {
        preview[i] = ram[address] | (unsigned)ram[address + 1] << 8;
        preview[i + 1] = ram[address + 2] | (unsigned)ram[address + 3] << 8;
        address = (address + (uint32_t)step) & 0x1ffffc;
    }
    m->preview = preview; m->preview_pos = 0; m->preview_count = m->remaining;
}
static int pixels(mdec_t *m) {
    unsigned i;
    if (complex_input) return 0;
    if (output_depth == 2) memset(m->output, 128, 192);
    else for (i = 0; i < 128; i += 2) { m->output[i] = 0x10; m->output[i+1] = 0x42; }
    m->output_pos = 0; m->output_size = output_depth == 2 ? 192 : 128; m->output_block = m->block;
    ++pixel_calls; return 1;
}
static const mdec_accel_t accelerator = {prepare, cancel, pixels};
static cpu_t *create(unsigned accelerated) {
    cpu_t *c = calloc(1, sizeof(*c));
    unsigned i, count = complex_input ? 180 : 120;
    assert(c); cpu_init(c); hw_get(c);
    if (accelerated) c->hw->mdec.accel = &accelerator;
    hw_write(c, 0x1f801824, 4, 0xe0000000);
    hw_write(c, 0x1f801820, 4, 0x40000001);
    for (i = 0; i < 32; ++i) hw_write(c, 0x1f801820, 4, 0x10101010);
    /* Constant standard DC row suffices for the zero and DC-only vectors. */
    hw_write(c, 0x1f801820, 4, 0x60000000);
    for (i = 0; i < 32; ++i) hw_write(c, 0x1f801820, 4, i < 4 ? 0x5a825a82 : 0);
    hw_write(c, 0x1f8010f0, 4, 0x88);
    hw_write(c, 0x1f8010f4, 4, 0x830000);
    for (i = 0; i < count; ++i) ram_write(c, 0x2000 + i * 4, 4, 0xfe000400);
    if (complex_input) {
        /* Dense signed ACs and qscale zero force restoration of an entire
         * verified prefix when the output backend declines its first Y. */
        for (i = 0; i < count * 2; ++i) {
            unsigned block = i / 3, part = i % 3;
            uint16_t value = part == 2 ? 0xfe00 : part ? (uint16_t)(block * 37 % 1024) :
                (uint16_t)((block % 2 ? 63u << 10 : 0) | (block * 73 % 1024));
            ram_write(c, 0x2000 + i * 2, 2, value);
        }
    }
    hw_write(c, 0x1f801820, 4, 0x20000000 | (output_depth << 27) | count);
    hw_write(c, 0x1f801080, 4, 0x2000);
    hw_write(c, 0x1f801084, 4, 0x10000 | count);
    hw_write(c, 0x1f801088, 4, 0x01000201);
    return c;
}
static void compare(cpu_t *a, cpu_t *b) {
    unsigned i;
    assert(a->cycles == b->cycles && a->mmio_epoch == b->mmio_epoch);
    assert(!memcmp(a->ram, b->ram, RAM_SIZE));
    assert(mdec_status(&a->hw->mdec) == mdec_status(&b->hw->mdec));
    assert(a->hw->mdec.due == b->hw->mdec.due);
    assert(a->hw->mdec.words_in == b->hw->mdec.words_in && a->hw->mdec.words_out == b->hw->mdec.words_out);
    assert(a->hw->mdec.blocks_decoded == b->hw->mdec.blocks_decoded);
    assert(a->hw->dicr == b->hw->dicr && a->hw->i_stat == b->hw->i_stat);
    assert(a->next_event == b->next_event);
    for (i = 0; i < 2; ++i) {
        assert(!memcmp(&a->hw->dma[i], &b->hw->dma[i], sizeof(a->hw->dma[i])));
        assert(!memcmp(&a->hw->media_dma[i], &b->hw->media_dma[i], sizeof(a->hw->media_dma[i])));
    }
}
int main(void) {
    unsigned mutate, i;
    for (mutate = 0; mutate < 8; ++mutate) {
        complex_input = mutate >= 3;
        cpu_t *a = create(0), *b = create(1);
        unsigned cancelled = 0;
        assert(b->hw->mdec.preview);
        /* Mutate chroma before DMA reads it, or Y3 after Y0 was accelerated.
         * The latter forces reconstruction of deferred chroma for fallback. */
        if (mutate && mutate != 3 && mutate != 7) {
            unsigned word = mutate == 1 || mutate == 4 ? 0 : mutate == 5 ? 1 : mutate == 6 ? 3 : 5;
            unsigned address = 0x2000 + word * 4;
            uint32_t value = mutate >= 4 ? (ram_read(a,address,4) & 65535) | (0x110u << 16) : 0xfe000408;
            ram_write(a, address, 4, value); ram_write(b, address, 4, value);
        }
        for (i = 0; i < 15000; ++i) {
            if (mutate == 7 && !cancelled && b->hw->mdec.raw_blocks) {
                b->hw->mdec.accel->cancel(&b->hw->mdec); cancelled = 1;
            }
            if (i == 2500) {
                unsigned j; cpu_t *cpus[2] = {a,b};
                for (j = 0; j < 2; ++j) {
                    hw_write(cpus[j], 0x1f801090, 4, 0x10000);
                    hw_write(cpus[j], 0x1f801094, 4, 0x10f00);
                    hw_write(cpus[j], 0x1f801098, 4, 0x01000200);
                }
            }
            a->cycles += 7; b->cycles += 7; hw_sync(a); hw_sync(b); compare(a,b);
        }
        assert(a->hw->dma[0].completions == 1 && a->hw->dma[1].completions == 1);
        if (mutate == 7) assert(cancelled);
        mdec_control(&a->hw->mdec, 0xe0000000); mdec_control(&b->hw->mdec, 0xe0000000);
        assert(!b->hw->mdec.preview); compare(a,b);
        hw_destroy(a); hw_destroy(b); free(a); free(b);
    }
    assert(pixel_calls >= 80 && cancel_calls);
    return 0;
}
