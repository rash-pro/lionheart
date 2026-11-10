
#include "cpu.h"
#include "generated.h"
#include <stdlib.h>
static uint64_t event_cycle;
void bios_call(cpu_t *c, unsigned t) { (void)c; (void)t; }
void bios_syscall(cpu_t *c) { (void)c; }
void gte_cmd(cpu_t *c, uint32_t v) { (void)c; (void)v; }
uint32_t gte_read(cpu_t *c, unsigned r, int t) { (void)t; return c->gte_data[r]; }
void gte_write(cpu_t *c, unsigned r, uint32_t v, int t) { (void)t; c->gte_data[r] = v; }
uint32_t hw_read(cpu_t *c, uint32_t a, unsigned w) { (void)a; (void)w; return (uint32_t)c->cycles; }
void hw_write(cpu_t *c, uint32_t a, unsigned w, uint32_t v) { (void)c; (void)a; (void)w; (void)v; }
void hw_event(cpu_t *c) { event_cycle = c->cycles; c->r[9] = 77; c->next_event = UINT64_MAX / 2; }
void check(const void *data, unsigned deadline, uint32_t *out) {
    cpu_t *c = malloc(sizeof(*c)); cpu_init(c); memcpy(c->ram + 0x10000, data, 36);
    c->entries = generated_entries; c->entry_count = generated_count; c->entry_regions = generated_regions;
    c->r[20] = 0x1f801814; c->instruction_limit = c->stall_limit = 0;
    c_set_sr(c, 0x401); c->next_event = deadline; event_cycle = 0;
    dispatch(c, 0x80010000);
    out[0] = c->r[8]; out[1] = (uint32_t)event_cycle; out[2] = (uint32_t)c->cycles; out[3] = c->r[9];
    free(c);
}
