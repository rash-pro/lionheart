
#include "hw.h"
#include "generated.h"
#include <assert.h>
#include <stdlib.h>
void check(const void *data, unsigned interpret, unsigned control, unsigned media,
           unsigned phase, uint64_t *out) {
    static const spu_backend_t sink = {NULL, NULL};
    cpu_t *c = malloc(sizeof(*c));
    assert(c); cpu_init(c); hw_get(c);
    memcpy(c->ram + 0x10000, data, DATA_SIZE);
    c->entries = generated_entries; c->entry_count = generated_count;
    c->entry_regions = generated_regions;
    c->instruction_limit = 100000; c->stall_limit = 0;
    c->cycles = phase; hw_sync(c);
    if (control) spu_set_backend(c->hw->spu, &sink, NULL);
    mem_write32(c, 0x80180000, 1);
    if (media == 1) {
        hw_write(c, 0x1f801824, 4, 0xe0000000);
        hw_write(c, 0x1f801820, 4, 0x24000004);
        for (unsigned i = 0; i < 4; ++i) hw_write(c, 0x1f801820, 4, 0xfe000000);
        hw_write(c, 0x1f8010f0, 4, 0x88);
        hw_write(c, 0x1f801090, 4, 0x180000);
        hw_write(c, 0x1f801094, 4, 0x10020);
        hw_write(c, 0x1f801098, 4, 0x01000200);
        assert(c->ram_limit <= 0x180000 && c->hw->media_bound != HW_NEVER);
    } else if (media == 2) {
        hw_write(c, 0x1f801da6, 2, 0);
        hw_write(c, 0x1f8010f0, 4, 0x80000);
        hw_write(c, 0x1f8010c0, 4, 0x180000);
        hw_write(c, 0x1f8010c4, 4, 0x10001);
        hw_write(c, 0x1f8010c8, 4, 0x01000200);
    } else {
        c->hw->events[0] = (bios_event_t){0xf000000au, 0x1000, 0x1000, 0x80010200, 2};
        c->hw->auto_ack |= 0x200;
        hw_write(c, 0x1f801da4, 2, 0);
        hw_write(c, 0x1f801daa, 2, 0xc040);
        hw_write(c, 0x1f801074, 2, 0x200);
        c_set_sr(c, 0x401);
    }
    c->interpreter_only = interpret;
    dispatch(c, 0x80010000);
    hw_sync(c);
    for (unsigned i = 0; i < 32; ++i) out[i] = c->r[i];
    out[32] = c->pc; out[33] = c->cycles; out[34] = c->load_reg; out[35] = c->load_value;
    out[36] = mem_read32(c, 0x80180000); out[37] = c_instructions(c); out[38] = c->halted;
    out[39] = c->hw->irq_delivered[9]; out[40] = c->hw->irq_raised[9];
    out[41] = c->hw->dma[1].madr; out[42] = c->hw->dma[1].chcr;
    out[43] = c->hw->mdec.words_out; out[44] = spu_read(c->hw->spu, 0x1ae);
    for (unsigned i = 0; i < 32; ++i) out[45 + i] = c->cop0[i];
    out[77] = c->idle_instructions; out[78] = c->hw->dma[4].madr;
    hw_destroy(c); free(c);
}
