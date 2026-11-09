
#include "hw.h"
#include "generated.h"
#include <stdlib.h>
void check(const void *data, unsigned interpret, unsigned target, uint64_t *out) {
    cpu_t *c = malloc(sizeof(*c));
    cpu_init(c); memcpy(c->ram + 0x10000, data, DATA_SIZE);
    c->instruction_limit = c->stall_limit = 0; hw_get(c);
    c->entries = generated_entries; c->entry_count = generated_count; c->entry_regions = generated_regions;
    c->r[20] = 0x80100000; c->r[9] = UINT32_MAX; c->r[10] = 1;
    mem_write32(c, 0x80100000, 100000);
    c->hw->events[0] = (bios_event_t){0xf2000002u, 2, 0x1000, 0x80010100, 2};
    hw_write(c, 0x1f801114, 2, 0x100); /* Timer 1 clocks HBlank, masked. */
    hw_write(c, 0x1f801128, 2, target);
    hw_write(c, 0x1f801124, 2, 0x58);
    hw_write(c, 0x1f801074, 2, 0x40); c_set_sr(c, 0x401);
    c->interpreter_only = interpret;
    dispatch(c, 0x80010000);
    for (unsigned i = 0; i < 32; ++i) out[i] = c->r[i];
    out[32] = c->pc; out[33] = c->cycles; out[34] = c->load_reg; out[35] = c->load_value;
    out[36] = mem_read32(c, 0x80100000); out[37] = mem_read32(c, 0x80100004);
    out[38] = c_instructions(c); out[39] = c->halted;
    out[40] = c->interp_instructions; out[41] = c->idle_instructions;
    out[42] = c->hw->irq_delivered[6]; out[43] = c->hw->event_calls[HW_VIDEO];
    hw_destroy(c); free(c);
}
