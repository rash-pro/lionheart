
#include "cpu.h"
#include "generated.h"
#include <stdlib.h>
void bios_call(cpu_t *c, unsigned t) { (void)c; (void)t; }
void bios_syscall(cpu_t *c) { (void)c; }
void hw_event(cpu_t *c) { c->next_event = UINT64_MAX/2; }
uint32_t hw_read(cpu_t *c, uint32_t a, unsigned w) { (void)c; (void)a; (void)w; return 0; }
void hw_write(cpu_t *c, uint32_t a, unsigned w, uint32_t v) { (void)c; (void)a; (void)w; (void)v; }
void gte_cmd(cpu_t *c, uint32_t v) { (void)c; (void)v; }
uint32_t gte_read(cpu_t *c, unsigned r, int t) { (void)c; (void)r; (void)t; return 0; }
void gte_write(cpu_t *c, unsigned r, uint32_t v, int t) { (void)c; (void)r; (void)v; (void)t; }
void check(const void *data, unsigned interpret) {
    cpu_t *c = malloc(sizeof(*c)); cpu_init(c); memcpy(c->ram + 0x10000, data, 20);
    c->entries = generated_entries; c->entry_count = generated_count; c->entry_regions = generated_regions;
    c->r[8] = 3; c->interpreter_only = interpret; c->next_event = UINT64_MAX/2;
    c_perf_checkpoint(c, 10); dispatch(c, 0x80010000); c_perf_checkpoint(c, 11);
    free(c);
}
