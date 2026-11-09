
#include "cpu.h"
#include <stdlib.h>
void bios_call(cpu_t *c, unsigned t) { (void)c; (void)t; }
void bios_syscall(cpu_t *c) { (void)c; }
void hw_event(cpu_t *c) { c->next_event = UINT64_MAX / 2; }
uint32_t hw_read(cpu_t *c, uint32_t a, unsigned w) { (void)c; (void)a; (void)w; return 0; }
void hw_write(cpu_t *c, uint32_t a, unsigned w, uint32_t v) { (void)c; (void)a; (void)w; (void)v; }
void gte_cmd(cpu_t *c, uint32_t v) { (void)c; (void)v; }
uint32_t gte_read(cpu_t *c, unsigned r, int t) { (void)c; (void)r; (void)t; return 0; }
void gte_write(cpu_t *c, unsigned r, uint32_t v, int t) { (void)c; (void)r; (void)v; (void)t; }
void check(uint64_t *out) {
    cpu_t *c = malloc(sizeof(*c));
    const code_span_t span = {0x80010000, 4};
    const code_region_t region = {&span, 1, 0, 0, NULL};
    memory_site_t site = {0x80010000, 0, 0, 0, 0, 8, 9, 35, 1, &region};
    cpu_init(c); c->next_event = UINT64_MAX/2;
    c->r[8] = 0x80100000; c_memory(c, &site, 0, 0);
    c->r[8] = 0x1f800000; c_memory(c, &site, 0, 0);
    c->r[8] = 0x1f801070; c_memory(c, &site, 0, 0);
    c->r[8] = 0x80100000; c->next_event = c->cycles + 2; c_memory(c, &site, 0, 0);
    c->ram_top = c->ram_limit = 0x100000; c_memory(c, &site, 0, 0);
    c->ram_top = c->ram_limit = 0x800000; c->code_watched[0x100000 >> CODE_PAGE_SHIFT] = 1;
    site.op = 43; c_memory(c, &site, 0, 0);
    c_transfer_stats(c, 0x80010000, 0x80020000, C_DISPATCH_CALL);
    c_transfer_stats(c, 0x80020000, 0x80010008, C_DISPATCH_RETURN);
    c_transfer_stats(c, 0x80010020, 0x80010000, C_DISPATCH_TABLE);
    for (unsigned i = 0; i < C_MEM_COUNT; ++i) out[i] = c->perf.memory[i];
    for (unsigned i = 0; i < C_DISPATCH_COUNT; ++i) out[C_MEM_COUNT+i] = c->perf.dispatch[i];
    free(c);
}
