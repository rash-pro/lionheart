#ifndef FF8_TIMING_H
#define FF8_TIMING_H
#include "cpu.h"
#include "scratchpad.h"

/* Referenced only by --timing block|loose output. No exact-path switch. */
int c_timing_block(cpu_t *, uint32_t, uint32_t, const code_region_t *);
int c_timing_poll(cpu_t *, const code_region_t *, uint32_t);
/* Dispatch validated this invocation's source. An unchanged epoch and a future
 * event need no observer or source lookup. The slow arm sees exported state. */
static inline int c_timing_charge(cpu_t *c, uint32_t pc, uint32_t n,
                                  const code_region_t *region, uint64_t epoch,
                                  uint32_t mask) {
    c->pc = pc;
    c_count_recompiled(c, region->spans[0].address, n);
    c_recomp_budget(c, n);
    c->cycles += (uint64_t)n * CYCLES_PER_INSN;
    return epoch == c->code_epoch &&
        (int64_t)((c->cycles & ~(uint64_t)mask) - c->next_event) < 0;
}
void c_timing_idle(cpu_t *);
#endif
