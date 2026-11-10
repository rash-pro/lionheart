
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
int probe(const void *data) {
    cpu_t *c = malloc(sizeof(*c));
    int result;
    cpu_init(c);
    memcpy(c->ram + 0x10000, data, 20);
    c->entries = generated_entries; c->entry_count = generated_count;
    c->entry_regions = generated_regions;
    c->r[8] = 1000000; c->instruction_limit = 10000; c->stall_limit = 0;
    dispatch(c, 0x80010000);
    result = c->halted && !strcmp(c->stop_reason, "instruction budget exhausted") &&
             c->cycles >= 20000 && c->cycles < 20008 &&
             !((uintptr_t)c->ram & 15);
#ifdef FF8_STATS
    result &= c->recomp_instructions > 0 && c->recomp_functions > 0;
#else
    result &= !c->recomp_instructions && !c->recomp_functions;
#endif
    free(c); return result;
}
