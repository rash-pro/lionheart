
#include "cpu.h"
#include "generated.h"
#include <stdlib.h>
void bios_call(cpu_t *c, unsigned t) { (void)c; (void)t; }
void bios_syscall(cpu_t *c) { (void)c; }
void gte_cmd(cpu_t *c, uint32_t v) { (void)c; (void)v; }
uint32_t gte_read(cpu_t *c, unsigned r, int t) { (void)t; return c->gte_data[r]; }
void gte_write(cpu_t *c, unsigned r, uint32_t v, int t) { (void)t; c->gte_data[r] = v; }
uint32_t hw_read(cpu_t *c, uint32_t a, unsigned w) { (void)c; (void)a; (void)w; return 0; }
void hw_write(cpu_t *c, uint32_t a, unsigned w, uint32_t v) { (void)c; (void)a; (void)w; (void)v; }
void check(const void *data, uint32_t replacement, uint32_t *out) {
    cpu_t *c = malloc(sizeof(*c)); cpu_init(c); memcpy(c->ram + 0x10000, data, 24);
    c->entries = generated_entries; c->entry_count = generated_count; c->entry_regions = generated_regions;
    c->r[20] = 0x8001000c; c->r[8] = c->gte_data[23] = replacement;
    c->instruction_limit = c->stall_limit = 0; c->next_event = UINT64_MAX / 2;
    dispatch(c, 0x80010000);
    out[0] = c->r[9]; out[1] = (uint32_t)c->cycles; free(c);
}
