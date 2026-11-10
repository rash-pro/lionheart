
#include "cpu.h"
#include "generated.h"
#include <stdlib.h>
void bios_call(cpu_t *c, unsigned t) { (void)t; c->r[10] = 123; }
void bios_syscall(cpu_t *c) { (void)c; }
void gte_cmd(cpu_t *c, uint32_t v) { (void)c; (void)v; }
uint32_t gte_read(cpu_t *c, unsigned r, int t) { (void)c; (void)r; (void)t; return 0; }
void gte_write(cpu_t *c, unsigned r, uint32_t v, int t) { (void)c; (void)r; (void)v; (void)t; }
uint32_t hw_read(cpu_t *c, uint32_t a, unsigned w) { (void)c; (void)a; (void)w; return 0; }
void hw_write(cpu_t *c, uint32_t a, unsigned w, uint32_t v) { (void)c; (void)a; (void)w; (void)v; }
void hw_event(cpu_t *c) {
    c->cop0[13] |= 0x400;
    if (c->in_delay_slot || (c->cop0[12] & 0x401) != 0x401) { c->next_event = c->cycles + 2; return; }
#if defined(FF8_TEST_IRQ_ALU) || defined(FF8_TEST_IRQ_BIOS) || defined(FF8_TEST_IRQ_ENABLE) || defined(FF8_TEST_IRQ_RFE)
    mem_write32(c, 0x80100004, c->r[9]);
#else
    mem_write32(c, 0x80100004, mem_read32(c, 0x80100000));
#endif
    c->cop0[13] &= ~0x400u; c->next_event = UINT64_MAX / 2;
}
int check(const void *data, unsigned interpret) {
    cpu_t *c = malloc(sizeof(*c)); int result;
    cpu_init(c); memcpy(c->ram + 0x10000, data, DATA_SIZE); c_set_sr(c, 0x401);
#ifdef FF8_TEST_IRQ_ENABLE
    c_set_sr(c, 0); c->r[12] = 0x401;
#endif
#ifdef FF8_TEST_IRQ_RFE
    c_set_sr(c, 0x404);
#endif
    c->entries = generated_entries; c->entry_count = generated_count; c->entry_regions = generated_regions;
    c->r[20] = 0x80100000; mem_write32(c, 0x80100000, 55);
    c->interpreter_only = interpret; c->next_event = IRQ_CYCLE; c->instruction_limit = c->stall_limit = 0;
    dispatch(c, 0x80010000);
#ifdef FF8_TEST_IRQ_ALU
    result = !c->halted && c->cycles == 8 && c->r[9] == 77 && mem_read32(c, 0x80100004) == 0;
#elif defined(FF8_TEST_IRQ_BIOS)
    result = !c->halted && c->cycles == 12 && c->r[9] == 77 && c->r[10] == 123 && mem_read32(c, 0x80100004) == 77;
#elif defined(FF8_TEST_IRQ_ENABLE) || defined(FF8_TEST_IRQ_RFE)
    result = !c->halted && c->cycles == 10 && c->r[9] == 77 && mem_read32(c, 0x80100004) == 0;
#else
    result = !c->halted && c->cycles == 10 && mem_read32(c, 0x80100000) == 99 && mem_read32(c, 0x80100004) == 55;
#endif
    free(c); return result;
}
