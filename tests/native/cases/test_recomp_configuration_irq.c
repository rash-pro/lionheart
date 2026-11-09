
#include "cpu.h"
#include "generated.h"
#include <stdlib.h>
void bios_call(cpu_t *c, unsigned t) { (void)c; (void)t; }
void bios_syscall(cpu_t *c) { (void)c; }
void gte_cmd(cpu_t *c, uint32_t v) { (void)c; (void)v; }
uint32_t gte_read(cpu_t *c, unsigned r, int t) { (void)c; (void)r; (void)t; return 0; }
void gte_write(cpu_t *c, unsigned r, uint32_t v, int t) { (void)t; c->gte_data[r] = v; }
uint32_t hw_read(cpu_t *c, uint32_t a, unsigned w) {
    (void)a; (void)w; c->next_event = c->cycles + 2; return 123;
}
void hw_write(cpu_t *c, uint32_t a, unsigned w, uint32_t v) { (void)c; (void)a; (void)w; (void)v; }
void hw_event(cpu_t *c) {
    uint32_t regs[32], pc = c->pc, load = c->load_reg, value = c->load_value;
    c->next_event = UINT64_MAX / 2;
    memcpy(regs, c->r, sizeof(regs));
    c->r[31] = 0xfffffff0u; c->load_reg = c->load_value = 0;
    c_enter(c, 0x80010030, 0xfffffff0u); dispatch(c, 0x80010030); c_leave(c);
    memcpy(c->r, regs, sizeof(regs)); c->pc = pc;
    c->load_reg = load; c->load_value = value;
}
int check(const void *data, unsigned interpret) {
    cpu_t *c = malloc(sizeof(*c));
    int result;
    cpu_init(c); memcpy(c->ram + 0x10000, data, 64);
    c->interpreter_only = interpret;
    c->entries = generated_entries; c->entry_count = generated_count;
    c->entry_regions = generated_regions;
    c->r[20] = 0x1f801000; c->r[21] = 0x80100000;
    c->next_event = UINT64_MAX / 2; c->instruction_limit = c->stall_limit = 0;
    dispatch(c, 0x80010000);
    result = !c->halted && c->pc == 0xffffffffu && c->r[9] == 2 && c->r[8] == 123 &&
             mem_read32(c, 0x80100000) == 2 && mem_read32(c, 0x80100004) == 1 &&
             (interpret ? c->interp_instructions == 10 :
              !c->interp_instructions && c->recomp_instructions == 10) && c->cycles == 20;
    free(c); return result;
}
