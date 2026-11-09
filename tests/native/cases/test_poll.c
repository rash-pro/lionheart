
#include "cpu.h"
#include "generated.h"
#include <stdlib.h>
#define FLAG FLAG_ADDRESS
static uint32_t flag(cpu_t *c) { return FLAG == 0x1f801070 ? c->mmio[0] : mem_read32(c, FLAG); }
void bios_call(cpu_t *c, unsigned t) { (void)c; (void)t; }
void bios_syscall(cpu_t *c) { (void)c; }
void gte_cmd(cpu_t *c, uint32_t v) { (void)c; (void)v; }
uint32_t gte_read(cpu_t *c, unsigned r, int t) { (void)c; (void)r; (void)t; return 0; }
void gte_write(cpu_t *c, unsigned r, uint32_t v, int t) { (void)c; (void)r; (void)v; (void)t; }
uint32_t hw_read(cpu_t *c, uint32_t a, unsigned w) { (void)a; (void)w; ++c->mmio_epoch; return c->mmio[0]; }
void hw_write(cpu_t *c, uint32_t a, unsigned w, uint32_t v) { (void)c; (void)a; (void)w; (void)v; }
void hw_event(cpu_t *c) {
    if (FLAG == 0x1f801070) c->mmio[0] = 0;
    else if (FLAG < 0x1f800000u || FLAG >= 0x1f800400u) ram_write(c, FLAG, 4, 0);
    else mem_write32(c, FLAG, 0);
    c->next_event = UINT64_MAX/2;
}
uint64_t check(const void *data, unsigned interpret, unsigned deadline, unsigned control, uint64_t *out) {
    cpu_t *c = malloc(sizeof(*c)); uint64_t accelerated;
    cpu_init(c); memcpy(c->ram + (BASE & (RAM_SIZE-1)), data, DATA_SIZE);
    c->entries = generated_entries; c->entry_count = generated_count; c->entry_regions = generated_regions;
    if (NATIVE_ABSENT) while (c->entry_count && c->entries[c->entry_count-1].pc >= GETTER) --c->entry_count;
    c->r[2] = 0x12345678; c->r[9] = 0x11223344; c->r[20] = 0x80100008;
    if (FLAG == 0x1f801070) c->mmio[0] = 1; else mem_write32(c, FLAG, 1);
    if (control == 2) c->ram_top = FLAG == 0x1f800000 ? 0 : 0x100000;
    c_set_sr(c, control == 1 ? 0x10401 : 0x401);
    if (control == 3) mem_write32(c, GETTER+12, 0xae890000); /* impure, runtime-replaced getter */
    c->interpreter_only = interpret; c->next_event = deadline;
    c->instruction_limit = control == 4 ? 101 : 0; c->stall_limit = 0;
    dispatch(c, BASE);
    for (unsigned i = 0; i < 32; ++i) out[i] = c->r[i];
    out[32] = c->pc; out[33] = c->cycles; out[34] = c->load_reg; out[35] = c->load_value;
    out[36] = flag(c); out[37] = mem_read32(c, 0x80100008); out[38] = c_instructions(c); out[39] = c->halted;
    for (unsigned i = 0; i < 32; ++i) out[40+i] = c->cop0[i];
    out[72] = c->branch_pending; out[73] = c->branch_pending ? c->branch_target : 0; out[74] = c->mmio_epoch;
    #ifndef FF8_STATS
    if (!c->instruction_limit) out[38] = 0;
#endif
    accelerated = c->idle_instructions; free(c); return accelerated;
}
