
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
void hw_event(cpu_t *c) {
    if ((c->cop0[12] & 0x401) == 0x401) mem_write32(c, 0x1f8003f8, 0x89abcdef);
    c->next_event = UINT64_MAX/2;
}
void check(const void *data, unsigned interpret, unsigned deadline, unsigned alias, unsigned sr, uint32_t *out) {
    cpu_t *c = malloc(sizeof(*c)); cpu_init(c); memcpy(c->ram + 0x10000, data, 48);
    c->entries = generated_entries; c->entry_count = generated_count; c->entry_regions = generated_regions;
    c->r[20] = alias; c->r[9] = 0x1234fedc; c_set_sr(c, sr);
    c->load_reg = 8; c->load_value = 0x87654321;
    c->interpreter_only = interpret; c->next_event = deadline; c->instruction_limit = c->stall_limit = 0;
    dispatch(c, 0x80010000);
    memcpy(out, c->r, 128); memcpy(out+32, c->cop0, 128); memcpy(out+64, c->gte_data, 128);
    out[96] = c->pc; out[97] = c->cycles; out[98] = c->load_reg; out[99] = c->load_value;
    out[100] = c_instructions(c); out[101] = c->halted; out[102] = c->branch_pending;
    memcpy(out+103, c->scratch, 1024); free(c);
}
