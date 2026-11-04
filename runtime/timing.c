#include "timing.h"

extern const uint32_t generated_timing_event_mask __attribute__((weak));

int c_timing_poll(cpu_t *c, const code_region_t *region, uint32_t mask) {
    if ((int64_t)((c->cycles & ~(uint64_t)mask) - c->next_event) >= 0) {
        hw_event(c);
        c_tick_slow(c);
    }
    return code_valid(c, region);
}

int c_timing_block(cpu_t *c, uint32_t pc, uint32_t n, const code_region_t *region) {
    uint32_t mask = &generated_timing_event_mask ? generated_timing_event_mask : 0;
    c->pc = pc;
    c_count_recompiled(c, region->spans[0].address, n);
    c_recomp_budget(c, n);
    c->cycles += (uint64_t)n * CYCLES_PER_INSN;
    if ((int64_t)((c->cycles & ~(uint64_t)mask) - c->next_event) >= 0) {
        hw_event(c);
        c_tick_slow(c);
    }
    return c->recent_region == region && c->recent_epoch == c->code_epoch ?
        c->recent_valid : code_valid(c, region);
}

/* Deliberately assumes a repeated short load loop is waiting for hardware.
 * It neither proves RAM ownership nor simulates the skipped register writes.
 * Bound each guess to one event and leave pending load state alone. */
void c_timing_idle(cpu_t *c) {
    uint64_t delta;
    poll_t *hint = &c->polls[(c->pc >> 2) % POLL_SLOTS];
    if (hint->pc != c->pc || hint->epoch != c->mmio_epoch) {
        hint->pc = c->pc; hint->epoch = c->mmio_epoch; hint->hits = 0;
        return;
    }
    if (++hint->hits < 4) return;
    if (c->load_reg || c->branch_pending || c->next_event <= c->cycles) return;
    delta = c->next_event - c->cycles;
    if (delta > 0x100000u) return;
    hint->hits = 0;
    c->cycles += delta;
    c_tick(c, 0);
}
