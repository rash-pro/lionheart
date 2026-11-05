#include "cpu.h"
#include "hw.h"
#include <stdlib.h>

cpu_t *tags_new(void) {
    cpu_t *c = malloc(sizeof(*c));
    if (!c) return NULL;
    cpu_init(c); hw_get(c);
    if (!frame_tags_enable(c)) { hw_destroy(c); free(c); return NULL; }
    FRAME_ORACLE(if (!frame_oracle_enable(c)) { hw_destroy(c); free(c); return NULL; });
    c->interpreter_only = 1;
    c->instruction_limit = c->stall_limit = 0;
    c->next_event = UINT64_MAX / 2;
    return c;
}
void tags_free(cpu_t *c) { hw_destroy(c); free(c); }
void tags_reset(cpu_t *c) { frame_tags_reset(c); }
void tags_pause(cpu_t *c, unsigned paused) { frame_tags_pause(c, paused); }
void tags_video(cpu_t *c, unsigned frame, unsigned words) {
    c->hw->vblanks = frame; c->hw->mdec.words_out = words;
    frame_tags_present(c);
}
void tags_reg(cpu_t *c, unsigned r, uint32_t value) {
    c->r[r] = value; frame_tags_registers(c)->r[r] = 0;
}
uint32_t tags_value(cpu_t *c, unsigned r) { return c->r[r]; }
void tags_write(cpu_t *c, uint32_t address, uint32_t value) { mem_write32(c, address, value); }
uint32_t tags_read(cpu_t *c, uint32_t address) { return mem_read32(c, address); }
frame_tag_t tags_memory(cpu_t *c, uint32_t address, unsigned width) { return frame_tags_load(c, address, width); }
frame_tag_t tags_register(cpu_t *c, unsigned r) { return frame_tags_registers(c)->r[r]; }
frame_tag_t tags_gte(cpu_t *c, unsigned r, unsigned ctrl) { return frame_tags_gte_read(c, r, ctrl); }
void tags_step(cpu_t *c, uint32_t word) {
    mem_write32(c, 0x10000, word); c->pc = 0x80010000;
    interp_step(c);
}
unsigned tags_halted(cpu_t *c) { return c->halted; }
void tags_control(cpu_t *c, unsigned reg, uint32_t value) {
    tags_step(c, (15u << 26) | (8u << 16) | (value >> 16));
    tags_step(c, (13u << 26) | (8u << 21) | (8u << 16) | (value & 65535));
    tags_step(c, (18u << 26) | (6u << 21) | (8u << 16) | (reg << 11));
}
uint32_t tags_gte_value(cpu_t *c, unsigned r) { return gte_read(c, r, 0); }
void tags_dma(cpu_t *c, uint32_t address) {
    mem_write32(c, 0x1f8010f0, 0x07654b21);
    mem_write32(c, 0x1f8010a0, address);
    mem_write32(c, 0x1f8010a8, 0x01000401);
    c->cycles += 64; hw_event(c);
    c->next_event = UINT64_MAX / 2;
}
void tags_bios(cpu_t *c, unsigned table) { bios_call(c, table); }
void tags_thread(cpu_t *c, unsigned restore, uint32_t address) {
    if (restore) bios_thread_restore(c, address); else bios_thread_save(c, address, c->pc, c->cop0[12]);
}
void tags_frame(cpu_t *c, unsigned start) {
    gpu_gp1(c->hw->gpu, 0x05000000u | start); gpu_vblank(c->hw->gpu);
}
int tags_corners(cpu_t *c, unsigned second, unsigned unknown, frame_corner_t out[3]) {
    uint32_t words[4] = {c->gte_data[12], c->gte_data[13], c->gte_data[14], c->gte_data[12]};
    frame_tag_t tags[4];
    unsigned i;
    for (i = 0; i < 4; ++i) tags[i] = unknown & (1u << i) ? FRAME_TAG_UNKNOWN : frame_tags_gte_read(c, 12 + i % 3, 0);
    frame_tags_polygon(c, 0x28, words, tags, 4, 0, 0);
    return frame_tags_triangle(c, second, out);
}
