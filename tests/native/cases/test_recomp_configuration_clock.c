
#include "cpu.h"
#include "generated.h"
#include <stdlib.h>
void bios_call(cpu_t *c, unsigned t) { (void)c; (void)t; }
void bios_syscall(cpu_t *c) { (void)c; }
void gte_cmd(cpu_t *c, uint32_t v) { (void)c; (void)v; }
uint32_t gte_read(cpu_t *c, unsigned r, int t) { (void)c; (void)r; (void)t; return 0; }
void gte_write(cpu_t *c, unsigned r, uint32_t v, int t) { (void)c; (void)r; (void)v; (void)t; }
uint32_t hw_read(cpu_t *c, uint32_t a, unsigned w) { (void)a; (void)w; return c->cycles; }
void hw_write(cpu_t *c, uint32_t a, unsigned w, uint32_t v) { (void)c; (void)a; (void)w; (void)v; }
void hw_event(cpu_t *c) { c->mmio_epoch = c->cycles; c->next_event = UINT64_MAX / 2; }
int check(const void *data, unsigned interpret) {
    cpu_t *c = malloc(sizeof(*c));
    int result;
    cpu_init(c); memcpy(c->ram + 0x10000, data, 40);
    c->entries = generated_entries; c->entry_count = generated_count;
    c->entry_regions = generated_regions;
    c->r[8] = 3; c->r[20] = 0x1f801000; c->r[21] = 0x80100000;
    c->instruction_limit = 10000; c->stall_limit = 0;
    c->interpreter_only = interpret; c->next_event = 10; c_set_sr(c, 0x401);
    dispatch(c, 0x80010000);
    result = !c->halted && c->mmio_epoch == 10 && c->cycles == 30 &&
             mem_read32(c, 0x80100000) == 20 &&
             c_instructions(c) == 15 && (interpret || c->recomp_instructions > 0);
    free(c); return result;
}
