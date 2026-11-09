
#include "cpu.h"
#include "generated.h"
#include <stdlib.h>
void bios_call(cpu_t *c, unsigned t) { (void)c; (void)t; }
void bios_syscall(cpu_t *c) { (void)c; }
void gte_cmd(cpu_t *c, uint32_t v) { (void)c; (void)v; }
uint32_t gte_read(cpu_t *c, unsigned r, int t) { (void)t; return c->gte_data[r]; }
void gte_write(cpu_t *c, unsigned r, uint32_t v, int t) { (void)c; (void)r; (void)v; (void)t; }
uint32_t hw_read(cpu_t *c, uint32_t a, unsigned w) { (void)c; (void)a; (void)w; return 0; }
void hw_write(cpu_t *c, uint32_t a, unsigned w, uint32_t v) { (void)c; (void)a; (void)w; (void)v; }
void hw_event(cpu_t *c) { mem_write32(c, 0x80100000, 123); c->gte_data[8] = 123; c->next_event = UINT64_MAX / 2; }
int check(const void *data, unsigned interpret) {
    cpu_t *c = malloc(sizeof(*c));
    int result;
    cpu_init(c); memcpy(c->ram + 0x10000, data, DATA_BYTES);
    c->entries = generated_entries; c->entry_count = generated_count; c->entry_regions = generated_regions;
    c->r[20] = 0x80100000; mem_write32(c, 0x80100000, 55);
    c->gte_data[8] = 99;
    c->interpreter_only = interpret; c->next_event = 2; c->instruction_limit = c->stall_limit = 0;
    dispatch(c, 0x80010000);
    result = !c->halted && c->pc == 0xffffffffu && c->cycles == EXPECTED_CYCLES &&
             mem_read32(c, CHECK_ADDRESS) == EXPECTED_VALUE;
    free(c); return result;
}
