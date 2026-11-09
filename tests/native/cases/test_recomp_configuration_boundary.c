
#include "cpu.h"
#include "generated.h"
#include <stdlib.h>
void bios_call(cpu_t *c, unsigned t) { (void)c; (void)t; }
void bios_syscall(cpu_t *c) { (void)c; }
void gte_cmd(cpu_t *c, uint32_t v) { (void)c; (void)v; }
uint32_t gte_read(cpu_t *c, unsigned r, int t) { (void)c; (void)r; (void)t; return 0; }
void gte_write(cpu_t *c, unsigned r, uint32_t v, int t) { (void)c; (void)r; (void)v; (void)t; }
uint32_t hw_read(cpu_t *c, uint32_t a, unsigned w) {
    (void)a; (void)w;
    if (c->seed) c->next_event = c->cycles + c->seed;
    return 123;
}
void hw_write(cpu_t *c, uint32_t a, unsigned w, uint32_t v) { (void)c; (void)a; (void)w; (void)v; }
void hw_event(cpu_t *c) {
    uint32_t *record = (uint32_t *)c->scratch;
    unsigned offset = 1 + record[0] * 38;
    if (++record[0] > 2) abort();
    memcpy(record + offset, c->r, sizeof(c->r));
    record[offset + 32] = c->pc; record[offset + 33] = c->cycles;
    record[offset + 34] = c->load_reg; record[offset + 35] = c->load_value;
    record[offset + 36] = c->branch_pending;
    record[offset + 37] = c->branch_pending ? c->branch_target : 0;
    c->next_event = UINT64_MAX / 2;
}
int check(const void *data, unsigned interpret, unsigned deadline, unsigned scheduled, uint32_t *out) {
    cpu_t *c = malloc(sizeof(*c));
    cpu_init(c); memcpy(c->ram + 0x10000, data, DATA_SIZE);
    c->entries = generated_entries; c->entry_count = generated_count; c->entry_regions = generated_regions;
    c->r[20] = 0x80100000; c->r[21] = 0x1f801000;
    mem_write32(c, 0x80100000, 77); mem_write32(c, 0x80100004, 88);
    c_set_sr(c, 0x401); c->seed = scheduled;
    c->interpreter_only = interpret; c->next_event = deadline;
    c->instruction_limit = c->stall_limit = 0;
    dispatch(c, 0x80010000);
    memcpy(out, c->scratch, 77 * sizeof(*out));
    memcpy(out + 77, c->r, sizeof(c->r));
    out[109] = c->pc; out[110] = c->cycles;
    out[111] = c->load_reg; out[112] = c->load_value; out[113] = c->branch_pending;
    out[114] = c_instructions(c);
    out[115] = c->interp_instructions; out[116] = c->recomp_instructions;
    int result = !c->halted && c->pc == 0xffffffffu;
    free(c); return result;
}
