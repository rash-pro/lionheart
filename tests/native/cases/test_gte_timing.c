#include "cpu.h"
#include "generated.h"
#include <stdlib.h>
static unsigned events;
void bios_call(cpu_t *c, unsigned t) { (void)c; (void)t; }
void bios_syscall(cpu_t *c) { (void)c; }
uint32_t hw_read(cpu_t *c, uint32_t a, unsigned w) { (void)c; (void)a; (void)w; return 0; }
void hw_write(cpu_t *c, uint32_t a, unsigned w, uint32_t v) { (void)c; (void)a; (void)w; (void)v; }
void hw_event(cpu_t *c) {
    ++events;
    gte_write(c, 7, 65535, 1);
    gte_write(c, 15, 0xfedf0123u, 0);
    c->next_event = UINT64_MAX / 2;
}
void check(const void *data, unsigned interpret, unsigned deadline, uint64_t *out) {
    cpu_t *c = malloc(sizeof(*c));
    cpu_init(c); memcpy(c->ram + 0x10000, data, DATA_BYTES);
    c->entries = generated_entries; c->entry_count = generated_count;
    c->entry_regions = generated_regions;
    c->r[8] = 0x8001fedcu; c->r[9] = 0x12345678; c->r[20] = 0x80100000;
    ram_write(c, 0x100000, 4, 0x00100020); ram_write(c, 0x100004, 4, 0x0040);
    c->load_reg = 8; c->load_value = 0x5555aaaa;
    c_set_sr(c, 0x401); c->next_event = deadline;
    c->instruction_limit = c->stall_limit = 0;
    c->interpreter_only = interpret; events = 0;
    dispatch(c, 0x80010000);
    for (unsigned i = 0; i < 32; ++i) {
        out[i] = c->r[i]; out[32+i] = c->gte_data[i]; out[64+i] = c->gte_ctrl[i];
    }
    out[96] = c->cycles; out[97] = c->gte_commands; out[98] = c->pc;
    out[99] = c->load_reg; out[100] = c->load_value; out[101] = c->halted;
    out[102] = events; out[103] = ram_read(c, 0x100008, 4);
    free(c);
}
