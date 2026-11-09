
#include "cpu.h"
#include "generated.h"
#include <stdlib.h>
void bios_call(cpu_t *c, unsigned t) { (void)c; (void)t; }
void bios_syscall(cpu_t *c) { (void)c; }
void gte_cmd(cpu_t *c, uint32_t v) { (void)c; (void)v; }
uint32_t gte_read(cpu_t *c, unsigned r, int t) { (void)c; (void)r; (void)t; return 0; }
void gte_write(cpu_t *c, unsigned r, uint32_t v, int t) { (void)c; (void)r; (void)v; (void)t; }
uint32_t hw_read(cpu_t *c, uint32_t a, unsigned w) { (void)c; (void)a; (void)w; return 0; }
void hw_write(cpu_t *c, uint32_t a, unsigned w, uint32_t v) { (void)c; (void)a; (void)w; (void)v; }
void hw_event(cpu_t *c) { mem_write32(c, 0x80100004, 1); c->next_event = UINT64_MAX/2; }
uint64_t check(const void *data, unsigned interpret, unsigned deadline, unsigned counter, unsigned alias, uint32_t *out) {
    cpu_t *c = malloc(sizeof(*c)); uint64_t accelerated;
    cpu_init(c); memcpy(c->ram + 0x10000, data, 72); c_set_sr(c, 0x401);
    c->entries = generated_entries; c->entry_count = generated_count; c->entry_regions = generated_regions;
    c->r[20] = alias ? 0x80100004 : 0x80100000; c->r[9] = UINT32_MAX; c->r[10] = 1;
    mem_write32(c, 0x80100000, counter); mem_write32(c, 0x80100004, 0);
    c->interpreter_only = interpret; c->next_event = deadline; c->instruction_limit = c->stall_limit = 0;
    dispatch(c, 0x80010000);
    memcpy(out, c->r, 128); out[32] = c->pc; out[33] = c->load_reg; out[34] = c->load_value;
    out[35] = c->cycles; out[36] = mem_read32(c, 0x80100000); out[37] = c_instructions(c);
    out[38] = mem_read32(c, 0x80100004); out[39] = c->halted;
    accelerated = c->idle_instructions; free(c); return accelerated;
}
