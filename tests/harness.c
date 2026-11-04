#include "cpu.h"
#include "generated.h"
#ifdef FF8_TEST_HARDWARE
#include "hw.h"
#endif
#include <stdlib.h>
#include <string.h>

cpu_t *test_new(void) {
    cpu_t *c = malloc(sizeof(*c));
    if (c) {
        cpu_init(c);
#ifdef FF8_TEST_HARDWARE
        /* The host initializes BIOS hardware before guest cache-isolation code. */
        hw_get(c);
#endif
        c->entries = generated_entries; c->entry_count = generated_count;
#ifdef GENERATED_HAS_OVERLAYS
    c->entry_regions = generated_regions;
        c->overlays = generated_overlays; c->overlay_count = generated_overlay_count;
#endif
        c->stall_limit = 10000; c->instruction_limit = 100000;
    }
    return c;
}
void test_free(cpu_t *c) {
#ifdef FF8_TEST_HARDWARE
    hw_destroy(c);
#endif
    free(c);
}
void test_register(cpu_t *c, unsigned reg, uint32_t value) { if (reg) c->r[reg] = value; }
void test_write(cpu_t *c, uint32_t address, uint32_t value, unsigned width) {
    if (width == 1) mem_write8(c, address, value);
    else if (width == 2) mem_write16(c, address, value);
    else mem_write32(c, address, value);
}
uint32_t test_read(cpu_t *c, uint32_t address, unsigned width) {
    return width == 1 ? mem_read8(c, address) : width == 2 ? mem_read16(c, address) : mem_read32(c, address);
}
void test_image(cpu_t *c, uint32_t address, const void *data, size_t size) {
    code_invalidate(c, address, size);
    memcpy(c->ram + (address & (RAM_SIZE - 1)), data, size);
}
void test_run(cpu_t *c, uint32_t pc, unsigned interpret) {
    c->interpreter_only = interpret;
    c->stop_armed = 1;
    if (!setjmp(c->stop)) {
        c_enter(c, pc, 0xffffffffu);
        dispatch(c, pc);
    }
    c->stop_armed = 0;
}
void test_step(cpu_t *c, uint32_t pc) { c->pc = pc; interp_step(c); }
void test_bios(cpu_t *c, unsigned table) { bios_call(c, table); }
void test_syscall(cpu_t *c) { bios_syscall(c); }
void test_snapshot(cpu_t *c, uint32_t *out) {
    memcpy(out, c->r, sizeof(c->r));
    out[32] = c->hi; out[33] = c->lo; out[34] = c->pc;
    out[35] = c->load_reg; out[36] = c->load_value;
    out[37] = c->halted; out[38] = c->branch_pending;
    memcpy(out + 39, c->cop0, sizeof(c->cop0));
    memcpy(out + 71, c->gte_data, sizeof(c->gte_data));
    memcpy(out + 103, c->gte_ctrl, sizeof(c->gte_ctrl));
}
uint64_t test_count(cpu_t *c, unsigned which) {
    switch (which) {
    case 0: return c->interp_instructions;
    case 1: return c->recomp_functions;
    case 2: return c->dispatch_misses;
    case 3: return c->gte_commands;
    case 5: return c->cycles;
    default: return c->recomp_instructions;
    }
}
const char *test_reason(cpu_t *c) { return c->stop_reason; }
uint8_t *test_ram(cpu_t *c) { return c->ram; }
uint64_t test_code_epoch(cpu_t *c) { return c->code_epoch; }
void test_registry_count(cpu_t *c, unsigned overlays, size_t count) {
    if (overlays) c->overlay_count = count; else c->entry_count = count;
}
void test_logger(cpu_t *c, void (*log)(void *, const char *)) { c->log = log; }
void test_gte_write(cpu_t *c, unsigned reg, uint32_t value, int control) { gte_write(c, reg, value, control); }
uint32_t test_gte_read(cpu_t *c, unsigned reg, int control) { return gte_read(c, reg, control); }
void test_poll(cpu_t *c, uint32_t pc, uint64_t limit) { c->pc = pc; c->stall_limit = limit; c_poll(c); }

void test_run_legacy(cpu_t *c, uint32_t pc) {
    c_enter(c, pc, 0xffffffffu);
    dispatch(c, pc);
}
void test_limits(cpu_t *c, uint64_t instructions, uint64_t stall) {
    c->instruction_limit = instructions;
    c->stall_limit = stall;
}
