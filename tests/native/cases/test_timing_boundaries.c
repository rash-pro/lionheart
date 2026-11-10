
#include "cpu.h"
#include "generated.h"
#include <stdlib.h>
static uint64_t event_cycle;
static uint64_t fence_cycle;
void bios_call(cpu_t *c, unsigned t) { (void)c; (void)t; }
void bios_syscall(cpu_t *c) { (void)c; }
void gte_cmd(cpu_t *c, uint32_t v) { (void)c; (void)v; }
uint32_t gte_read(cpu_t *c, unsigned r, int t) { (void)t; return c->gte_data[r]; }
void gte_write(cpu_t *c, unsigned r, uint32_t v, int t) { (void)t; c->gte_data[r] = v; }
uint32_t hw_read(cpu_t *c, uint32_t a, unsigned w) { (void)a; (void)w; return (uint32_t)c->cycles; }
void hw_write(cpu_t *c, uint32_t a, unsigned w, uint32_t v) { (void)c; (void)a; (void)w; (void)v; }
void hw_event(cpu_t *c) {
    event_cycle = c->cycles; c->r[9] = 77; ram_write(c, 0x100000, 4, 99); c->next_event = UINT64_MAX/2;
}
void hw_ram_fence(cpu_t *c, uint32_t p, uint32_t n) { (void)p; (void)n; fence_cycle = c->cycles; }
void check(const void *data, unsigned address, unsigned deadline, unsigned fenced, uint32_t *out) {
    cpu_t *c = malloc(sizeof(*c)); cpu_init(c); memcpy(c->ram + 0x10000, data, 32);
    c->entries = generated_entries; c->entry_count = generated_count; c->entry_regions = generated_regions;
    c->r[20] = address; c->instruction_limit = c->stall_limit = 0;
    if (fenced) c->ram_top = 0x100000;
    c_set_sr(c, 0x401); c->next_event = deadline; event_cycle = fence_cycle = 0;
    dispatch(c, 0x80010000);
    out[0] = c->r[8]; out[1] = (uint32_t)event_cycle; out[2] = (uint32_t)c->cycles;
    out[3] = (uint32_t)fence_cycle; out[4] = c->r[9]; free(c);
}
