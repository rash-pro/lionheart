#include "hw.h"
#include <assert.h>
#ifdef FF8_TEST_CD_LIMIT
void *__real_calloc(size_t, size_t);
void *__wrap_calloc(size_t count, size_t bytes) {
    if (count > 100000 && bytes == sizeof(uint32_t)) return NULL;
    return __real_calloc(count, bytes);
}
#endif
#include <stdlib.h>
#include <string.h>

static cpu_t *c;
static void advance(uint64_t n) { c->cycles += n; hw_event(c); }
static void write8(unsigned port, unsigned value) { hw_write(c, 0x1f801800 + port, 1, value); }
static unsigned read8(unsigned port) { return hw_read(c, 0x1f801800 + port, 1); }
static unsigned flags(void) { write8(0, 1); return read8(3) & 7; }
static void ack(void) { write8(0, 1); write8(3, 0x1f); hw_write(c, 0x1f801070, 2, ~4u); }
static void cmd(unsigned op, const uint8_t *params, unsigned n) {
    unsigned i;
    write8(0, 0);
    for (i = 0; i < n; ++i) write8(2, params[i]);
    write8(1, op);
}
static uint32_t bios(unsigned table, unsigned fn, uint32_t a, uint32_t b, uint32_t n, uint32_t d) {
    c->r[9] = fn; c->r[4] = a; c->r[5] = b; c->r[6] = n; c->r[7] = d; bios_call(c, table); return c->r[2];
}
static void disc(const char *path) { char error[256]; if (!cdrom_open(c, path, error, sizeof(error))) { fprintf(stderr, "%s\n", error); abort(); } }

static void cd_commands(const char *path) {
    uint8_t mode = 0xa0, location[] = {0, 2, 1}, version = 0x20;
    disc(path);
    write8(0, 1); write8(2, 0x1f);
    cmd(1, NULL, 0);
    assert(read8(0) & 0x80);
    advance(24999); assert(flags() == 0);
    advance(1); assert(flags() == 3 && read8(1) == 2 && (c->hw->i_stat & 4)); ack();
    cmd(0x19, &version, 1); advance(25000);
    assert(flags() == 3 && read8(1) == 0x98 && read8(1) == 6 && read8(1) == 0x10 && read8(1) == 0xc3);
    assert(!(read8(0) & 0x20)); ack();
    cmd(0xa, NULL, 0); advance(25000); assert(flags() == 3);
    advance(HW_CPU_HZ / 10); assert(flags() == 3); ack(); advance(1); assert(flags() == 2); ack();
    cmd(2, location, 3); advance(25000); ack();
    cmd(0xe, &mode, 1); advance(25000); ack();
    cmd(6, NULL, 0); advance(25000); assert(flags() == 3); ack();
    advance(HW_CPU_HZ / 150 - 1); assert(flags() == 0);
    advance(1); assert(flags() == 1 && c->hw->cd.sector_lba == 1);
    write8(0, 0); write8(3, 0x80); assert(read8(0) & 0x40);
    for (unsigned i = 0; i < 2340; ++i) assert(read8(2) == (i + 12 == 15 ? 2u : (i + 12 + 1) & 255));
    assert(!(read8(0) & 0x40)); ack();
    cmd(9, NULL, 0); advance(25000); assert(flags() == 3); ack();
    advance(HW_CPU_HZ / 150); assert(flags() == 2 && !c->hw->cd.reading); ack();
    cmd(0xff, NULL, 0); advance(25000); assert(flags() == 5); assert(read8(1) & 1); assert(read8(1) == 0x40); ack();
    cmd(2, location, 2); advance(25000); assert(flags() == 5 && (read8(1) & 1) && read8(1) == 0x20);
}

static void cd_dma(const char *path) {
    uint8_t location[] = {0, 2, 0};
    disc(path);
    cmd(2, location, 3); advance(25000); ack();
    cmd(6, NULL, 0); advance(25000); ack(); advance(HW_CPU_HZ / 75);
    assert(flags() == 1);
    write8(0, 0); write8(3, 0x80);
    hw_write(c, 0x1f8010f0, 4, 8u << 12);
    hw_write(c, 0x1f8010f4, 4, (1u << 23) | (1u << 19));
    hw_write(c, 0x1f8010b0, 4, 0x10000);
    hw_write(c, 0x1f8010b4, 4, 512);
    hw_write(c, 0x1f8010b8, 4, 0x11000000);
    assert(hw_read(c, 0x1f8010b8, 4) & 0x1000000);
    advance(1040);
    for (unsigned i = 0; i < 2048; ++i) assert(mem_read8(c, 0x10000 + i) == ((i + 24) & 255));
    assert(!(hw_read(c, 0x1f8010b8, 4) & 0x1000000));
    assert((hw_read(c, 0x1f8010f4, 4) & 0x88000000u) == 0x88000000u && (c->hw->i_stat & 8));
    hw_write(c, 0x1f8010f7, 1, 8); assert(!(hw_read(c, 0x1f8010f4, 4) & 0x88000000u));
    assert(c->hw->cd.bytes_dma == 2048);
    assert(c->hw->cd.lba_dma[0] == 2048);
}

static void cd_status(const char *path) {
    uint8_t filter[] = {7, 3}, mode = 0x88, track = 1, loc[] = {0, 2, 4};
    disc(path);
    cmd(0xd, filter, 2); advance(25000); ack();
    cmd(0xe, &mode, 1); advance(25000); ack();
    cmd(0xf, NULL, 0); advance(25000);
    assert(flags() == 3 && read8(1) == 2 && read8(1) == mode && read8(1) == 0 && read8(1) == 7 && read8(1) == 3); ack();
    cmd(0x13, NULL, 0); advance(25000); assert(read8(1) == 2 && read8(1) == 1 && read8(1) == 1); ack();
    cmd(0x14, &track, 1); advance(25000); assert(read8(1) == 2 && read8(1) == 0 && read8(1) == 2); ack();
    cmd(2, loc, 3); advance(25000); ack();
    cmd(0x15, NULL, 0); advance(25000); assert(flags() == 3); ack(); advance(HW_CPU_HZ / 30);
    assert(flags() == 2 && c->hw->cd.read_lba == 4); ack();
    cmd(0x1a, NULL, 0); advance(25000); assert(flags() == 3); ack(); advance(HW_CPU_HZ / 30);
    assert(flags() == 2 && read8(1) == 2 && read8(1) == 0 && read8(1) == 0x20 && read8(1) == 0);
    assert(read8(1) == 'S' && read8(1) == 'C' && read8(1) == 'E' && read8(1) == 'A'); ack();
    cmd(0x1e, NULL, 0); advance(25000); ack(); advance(HW_CPU_HZ / 30); assert(flags() == 2); ack();
    cmd(0x1b, NULL, 0); advance(25000); ack(); advance(HW_CPU_HZ / 150); assert(flags() == 1); ack();
    cmd(0x10, NULL, 0); advance(25000); assert(flags() == 3);
    for (unsigned i = 0; i < 8; ++i) assert(read8(1) == (i == 3 ? 2 : i + 16));
}

static void no_disc(void) {
    cmd(1, NULL, 0); advance(25000); assert(flags() == 3 && read8(1) == 0x10); ack();
    cmd(6, NULL, 0); advance(25000); assert(flags() == 5 && (read8(1) & 1) && read8(1) == 0x80);
}

static void disc_swap(const char *path, const char *replacement) {
    char error[256];
    FILE *original;
    uint8_t location[] = {0, 2, 0};
    disc(path); original = c->hw->cd.file;
    assert(!cdrom_swap(c, "/does/not/exist.cue", error, sizeof(error)));
    assert(c->hw->cd.file == original && c->hw->cd.status == 2);
    cmd(6, NULL, 0); advance(25000); ack(); advance(HW_CPU_HZ / 75); ack();
    write8(0, 0); write8(3, 0x80); assert(read8(0) & 0x40);
    assert(cdrom_swap(c, replacement, error, sizeof(error)));
    assert(!c->hw->cd.reading && !c->hw->cd.playing && !(read8(0) & 0x40));
    assert(flags() == 5 && read8(1) == 0x11 && read8(1) == 8); ack();
    cmd(1, NULL, 0); advance(25000); assert(read8(1) & 0x10); ack();
    cmd(1, NULL, 0); advance(25000); assert(read8(1) & 0x10); ack();
    cmd(6, NULL, 0); advance(25000); assert(flags() == 5 && (read8(1) & 1) && read8(1) == 0x80); ack();
    advance(HW_CPU_HZ / 2); assert(!c->hw->cd.shell_open);
    cmd(1, NULL, 0); advance(25000); assert(read8(1) & 0x10); ack();
    cmd(1, NULL, 0); advance(25000); assert(!(read8(1) & 0x10)); ack();
    advance(HW_CPU_HZ / 2);
    cmd(0x1a, NULL, 0); advance(25000); assert(flags() == 3); ack();
    advance(HW_CPU_HZ / 30); assert(flags() == 2 && read8(1) == 2); ack();
    cmd(2, location, 3); advance(25000); ack();
    cmd(6, NULL, 0); advance(25000); ack(); advance(HW_CPU_HZ / 75);
    assert(flags() == 1 && c->hw->cd.sector_lba == 0);
    write8(0, 0); write8(3, 0x80);
    assert(read8(2) == 0xa5);
}

static void gpu_dma(void) {
    mem_write32(c, 0x10000, 0x020000ff); mem_write32(c, 0x10004, 0); mem_write32(c, 0x10008, 0x00010010);
    hw_write(c, 0x1f8010a0, 4, 0x10000); hw_write(c, 0x1f8010a4, 4, 3); hw_write(c, 0x1f8010a8, 4, 0x11000001);
    advance(100); assert(hw_read(c, 0x1f8010a8, 4) & 0x1000000);
    hw_write(c, 0x1f8010f0, 4, 8u << 8); advance(22);
    assert(!(hw_read(c, 0x1f8010a8, 4) & 0x1000000) && gpu_vram(c->hw->gpu)[0] == 0x1f);
    mem_write32(c, 0x11000, 0x03ffffff); mem_write32(c, 0x11004, 0x0200ff00);
    mem_write32(c, 0x11008, 0); mem_write32(c, 0x1100c, 0x00010010);
    hw_write(c, 0x1f8010a0, 4, 0x11000); hw_write(c, 0x1f8010a8, 4, 0x01000401); advance(32);
    assert(gpu_vram(c->hw->gpu)[0] == 0x3e0);
}

static unsigned order[8], order_count;
static void first(cpu_t *cpu) { order[order_count++] = 1; cpu->r[2] = 1; cpu->pc = cpu->r[31]; }
static void second(cpu_t *cpu) { order[order_count++] = 2; cpu->pc = cpu->r[31]; }
static void lower(cpu_t *cpu) { order[order_count++] = 3; cpu->r[2] = 0; cpu->pc = cpu->r[31]; }
static void callback(cpu_t *cpu) {
    mem_write32(cpu, 0x5000, mem_read32(cpu, 0x5000) + 1);
    cpu->r[8] = 0xdeadc0de; cpu->hi = 0x123; cpu->lo = 0x456; cpu->pc = cpu->r[31];
}
static void hook(cpu_t *cpu) {
    assert(cpu->r[2] == 1 && cpu->r[29] == 0x8000f000);
    hw_write(cpu, 0x1f801070, 2, 0);
    mem_write32(cpu, 0x5004, 1);
    bios(1, 0x17, 0, 0, 0, 0);
    abort();
}
static const entry_t entries[] = {{0x80010000, first}, {0x80010004, second}, {0x80010008, lower}, {0x8001000c, callback}, {0x80010010, hook}};
static void kernel_ram_chains(void) {
    uint32_t table = mem_read32(c, 0x100);
    assert(table && mem_read32(c, 0x104) == 4 * 8);
    c->entries = entries; c->entry_count = sizeof(entries) / sizeof(entries[0]);
    mem_write32(c, 0x6004, 0x80010004); mem_write32(c, 0x6008, 0x80010000);
    mem_write32(c, 0x6028, 0x80010008);
    assert(bios(2, 2, 2, 0x6000, 0, 0) == 0);
    assert(mem_read32(c, table + 2 * 8) == 0x6000);
    /* Psy-Q appends its card handler through the public RAM table. */
    mem_write32(c, 0x6000, 0x6020);
    c_set_sr(c, 0x401); hw_write(c, 0x1f801074, 2, 4); hw_irq_raise(c, 2); hw_event(c);
    assert(order_count == 3 && order[0] == 1 && order[1] == 2 && order[2] == 3);
    assert(bios(2, 3, 2, 0x6020, 0, 0) == 0 && mem_read32(c, 0x6000) == 0);
    assert(bios(2, 3, 2, 0x6000, 0, 0) == 0 && mem_read32(c, table + 2 * 8) == 0);
}
static uint32_t helper_thread;
static unsigned thread_syscall, thread_branch;
static void select_thread(cpu_t *cpu) {
    /* The SDK selects a TCB directly while handling the VBlank IRQ. */
    uint32_t base = mem_read32(cpu, 0x110), pcb = mem_read32(cpu, 0x108);
    uint32_t next = base + (helper_thread & 0xffff) * 0xc0;
    assert(mem_read32(cpu, next) == 0x4000);
    mem_write32(cpu, pcb, next);
    cpu->pc = cpu->r[31];
}
static void thread_main(cpu_t *cpu) {
    cpu->r[16] = 0x12345678; cpu->r[29] = 0x801fff00;
    cpu->hi = 55; cpu->lo = 66;
    cpu->r[7] = 99; cpu->load_reg = 7; cpu->load_value = 77;
    cpu->pc = 0x80020004;
    if (thread_branch) {
        mem_write32(cpu, 0x80022000, 0xac10500c); /* delay-slot store */
        mem_write32(cpu, 0x80022004, 0x0000000d); /* must be skipped by the pending branch */
        mem_write32(cpu, 0x80022008, 0x03e00008); mem_write32(cpu, 0x8002200c, 0);
        cpu->r[31] = 0xffffffffu; cpu->pc = 0x80022000;
        cpu->branch_pending = 1; cpu->branch_pc = 0x80021ffc;
        cpu->branch_target = 0x80022008; cpu->branch_kind = 0;
    }
    hw_irq_raise(cpu, 0); hw_event(cpu);
    assert(!"IRQ must enter the selected helper thread");
}
static void thread_helper(cpu_t *cpu) {
    assert(cpu->r[29] == 0x801e0000 && cpu->r[28] == 0x80050000);
    assert((cpu->cop0[12] & 0x401) == 0x401);
    mem_write32(cpu, 0x5000, 42); cpu->r[16] = 999;
    cpu->r[31] = 0x80020014;
    if (thread_syscall) {
        cpu->pc = 0x80020010; cpu->r[4] = 3; cpu->r[5] = mem_read32(cpu, 0x110);
        bios_syscall(cpu);
    } else bios(1, 0x10, 0xff000000, 0, 0, 0);
    assert(!"ChangeTh must resume the interrupted main thread");
}
static void thread_resumed(cpu_t *cpu) {
    assert(mem_read32(cpu, 0x5000) == 42 && cpu->r[16] == 0x12345678);
    assert(cpu->r[29] == 0x801fff00 && cpu->hi == 55 && cpu->lo == 66 && cpu->r[2] == 1);
    mem_write32(cpu, 0x80022000, 0xac085004); /* sw t0,5004(zero) */
    mem_write32(cpu, 0x80022004, 0xac075008); /* sw a3,5008(zero) */
    cpu->pc = 0x80022000;
    interp_step(cpu); interp_step(cpu);
    assert(mem_read32(cpu, 0x5004) == 99 && mem_read32(cpu, 0x5008) == 77);
    cpu->pc = 0xffffffffu;
}
static void kernel_ram_threads(void) {
    static const entry_t thread_entries[] = {
        {0x80020000, thread_main}, {0x80020008, thread_resumed},
        {0x80020010, thread_helper}, {0x80021000, select_thread}
    };
    uint32_t tcb, handle;
    assert(mem_read32(c, 0x10c) == 4 && mem_read32(c, 0x114) >= 2 * 0xc0);
    helper_thread = bios(1, 0xe, 0x80020010, 0x801e0000, 0x80050000, 0);
    assert(helper_thread == 0xff000001);
    tcb = mem_read32(c, 0x110) + 0xc0;
    /* OpenTh leaves SR untouched; software supplies exception-mode SR. */
    assert(mem_read32(c, tcb + 0x94) == 0);
    mem_write32(c, tcb + 0x94, 0x404);
    mem_write32(c, 0x80020004, 0x00e04021); /* interrupted load-delay slot: addu t0,a3,zero */
    c->entries = thread_entries; c->entry_count = sizeof(thread_entries) / sizeof(thread_entries[0]);
    handle = bios(1, 8, 0xf2000003, 2, 0x1000, 0x80021000);
    bios(1, 0xc, handle, 0, 0, 0);
    c_set_sr(c, 0x401); hw_write(c, 0x1f801074, 2, 1);
    assert(c_enter(c, 0x80020000, 0xffffffffu)); dispatch(c, 0x80020000); c_leave(c);
    if (thread_branch) assert(mem_read32(c, 0x500c) == 0x12345678 && c->r[7] == 77);
    assert(mem_read32(c, mem_read32(c, 0x108)) == mem_read32(c, 0x110));
    assert(mem_read32(c, tcb + 0x88) == 0x80020014);
    assert(bios(1, 0xf, helper_thread, 0, 0, 0) == 1 && mem_read32(c, tcb) == 0x1000);
}
static void events(void) {
    uint32_t handle, regs[32];
    c->entries = entries; c->entry_count = sizeof(entries) / sizeof(entries[0]);
    handle = bios(1, 8, 0xf3000001, 2, 0x2000, 0);
    bios(1, 7, 0xf3000001, 2, 0, 0); assert(!bios(1, 0xb, handle, 0, 0, 0));
    bios(1, 0xc, handle, 0, 0, 0); bios(1, 7, 0xf3000001, 2, 0, 0);
    assert(bios(1, 0xb, handle, 0, 0, 0) == 1 && !bios(1, 0xb, handle, 0, 0, 0));
    bios(1, 7, 0xf3000001, 2, 0, 0); bios(1, 0x20, 0xf3000001, 2, 0, 0); assert(!bios(1, 0xb, handle, 0, 0, 0));
    bios(1, 0xd, handle, 0, 0, 0); assert(!bios(1, 0xa, handle, 0, 0, 0)); bios(1, 9, handle, 0, 0, 0);
    handle = bios(1, 8, 0xf2000000, 2, 0x1000, 0x8001000c); bios(1, 0xc, handle, 0, 0, 0);
    mem_write32(c, 0x6004, 0x80010004); mem_write32(c, 0x6008, 0x80010000);
    mem_write32(c, 0x6028, 0x80010008);
    assert(bios(2, 2, 0, 0x6000, 0, 0) == 0); assert(bios(2, 2, 2, 0x6020, 0, 0) == 0);
    for (unsigned i = 1; i < 32; ++i) c->r[i] = i * 99;
    memcpy(regs, c->r, sizeof(regs)); c->hi = 55; c->lo = 66; c->pc = 0x80020000;
    c->load_reg = 8; c->load_value = 987; c_set_sr(c, 0x401);
    hw_write(c, 0x1f801074, 2, 0x10); hw_irq_raise(c, 4); hw_event(c);
    assert(order_count == 3 && order[0] == 1 && order[1] == 2 && order[2] == 3);
    assert(!memcmp(c->r, regs, sizeof(regs)) && c->hi == 55 && c->lo == 66 && c->pc == 0x80020000);
    assert(c->load_reg == 8 && c->load_value == 987 && c->cop0[12] == 0x401);
    assert(mem_read32(c, 0x5000) == 1 && !(c->hw->i_stat & 0x10) && c->hw->irq_delivered[4] == 1);
    bios(2, 3, 0, 0x6000, 0, 0); bios(2, 3, 2, 0x6020, 0, 0);
    mem_write32(c, 0x7000, 0x80010010); mem_write32(c, 0x7004, 0x8000f000);
    bios(1, 0x19, 0x7000, 0, 0, 0); hw_write(c, 0x1f801074, 2, 4); hw_irq_raise(c, 2); hw_event(c);
    assert(mem_read32(c, 0x5004) == 1 && !c->hw->in_exception && c->depth == 0 && c->dispatch_depth == 0);
}
static void timers(void) {
    hw_write(c, 0x1f801108, 2, 10); hw_write(c, 0x1f801104, 2, 0x58);
    advance(9); assert(!(c->hw->i_stat & 0x10)); advance(1); assert(c->hw->i_stat & 0x10);
    assert(hw_read(c, 0x1f801100, 2) == 0 && (hw_read(c, 0x1f801104, 2) & 0x800));
    assert(!(hw_read(c, 0x1f801104, 2) & 0x800)); hw_write(c, 0x1f801070, 2, ~0x10u);
    advance(10); assert(c->hw->i_stat & 0x10);
    hw_write(c, 0x1f801104, 2, 0x18); hw_write(c, 0x1f801070, 2, 0);
    advance(10); assert(c->hw->i_stat & 0x10); hw_write(c, 0x1f801070, 2, 0); advance(20); assert(!(c->hw->i_stat & 0x10));
    hw_write(c, 0x1f801124, 2, 0x200); advance(80); assert(hw_read(c, 0x1f801120, 2) == 10);
    hw_write(c, 0x1f801124, 2, 1); advance(100); assert(hw_read(c, 0x1f801120, 2) == 0);
    hw_write(c, 0x1f801114, 2, 0x100); advance(10000); assert(hw_read(c, 0x1f801110, 2) >= 4);
    advance(HW_CPU_HZ); assert(c->hw->vblanks >= 59 && c->hw->vblanks <= 61);
    assert(c->next_event > c->cycles);
}

static void pal_timing(void) {
    uint64_t before;
    hw_write(c, 0x1f801814, 4, 0x08000008);
    advance(HW_CPU_HZ);
    assert(c->hw->vblanks == 50);
    before = c->hw->vblanks;
    hw_write(c, 0x1f801814, 4, 0x08000000);
    advance(HW_CPU_HZ);
    assert(c->hw->vblanks - before >= 59 && c->hw->vblanks - before <= 60);
}

static void timer_modes(void) {
    for (unsigned mode = 0; mode < 4; ++mode) {
        while (c->hw->hblank) advance(c->hw->next_video - c->cycles);
        hw_write(c, 0x1f801104, 2, 1 | (mode << 1));
        advance(c->hw->next_video - c->cycles - 1);
        assert((hw_read(c, 0x1f801100, 2) != 0) == (mode < 2));
        advance(1);
        unsigned before = hw_read(c, 0x1f801100, 2);
        advance(100);
        unsigned after = hw_read(c, 0x1f801100, 2);
        assert(mode == 0 ? before == after : after > before);
    }
    hw_write(c, 0x1f801104, 2, 0x20); hw_write(c, 0x1f801100, 2, 0xfffe); advance(1);
    assert((hw_read(c, 0x1f801104, 2) & 0x1000) && (c->hw->i_stat & 0x10));
    hw_write(c, 0x1f801108, 2, 4); hw_write(c, 0x1f801104, 2, 0xd8);
    advance(4); assert(!(hw_read(c, 0x1f801104, 2) & 0x400));
    advance(4); assert(hw_read(c, 0x1f801104, 2) & 0x400);
    hw_write(c, 0x1f801104, 2, 0x100); advance(100);
    assert(hw_read(c, 0x1f801100, 2) > 10 && hw_read(c, 0x1f801100, 2) < 40);
}

static void wait_event(void) {
    uint32_t handle = bios(1, 8, 0xf2000002, 2, 0x2000, 0);
    bios(1, 0xc, handle, 0, 0, 0);
    hw_write(c, 0x1f801128, 2, 100); hw_write(c, 0x1f801124, 2, 0x18); hw_write(c, 0x1f801074, 2, 0x40);
    c_set_sr(c, 0x401);
    assert(bios(1, 0xa, handle, 0, 0, 0) == 1 && c->cycles >= 100 && c->hw->irq_delivered[6] == 1);
    assert(!bios(1, 0xb, handle, 0, 0, 0));
}

static void bios_pad(void) {
    bios(1, 0x12, 0x5000, 4, 0x5010, 4);
    bios(1, 0x5b, 1, 0, 0, 0); bios(1, 0x13, 0, 0, 0, 0);
    c_set_sr(c, 0); advance(600000); assert(mem_read32(c, 0x5000) == 0);
    c_set_sr(c, 0x401); hw_event(c);
    assert(mem_read32(c, 0x5000) == 0xffff4100 && mem_read32(c, 0x5010) == 0xffff4100);
    assert(!(c->hw->i_stat & 1));
    bios(1, 0x14, 0, 0, 0, 0); mem_write32(c, 0x5000, 0);
    advance(600000); assert(mem_read32(c, 0x5000) == 0);
}
static void misc(void) {
    uint32_t a, b, d;
    bios(0, 0x39, 0x80010000, 1024, 0, 0);
    a = bios(0, 0x33, 64, 0, 0, 0); b = bios(0, 0x33, 64, 0, 0, 0);
    bios(0, 0x34, a, 0, 0, 0); bios(0, 0x34, b, 0, 0, 0);
    d = bios(0, 0x33, 1024, 0, 0, 0); assert(d == a && !bios(0, 0x33, 1, 0, 0, 0));
    hw_write(c, 0x1f8010f0, 4, 8u << 24); hw_write(c, 0x1f8010e0, 4, 0x200c);
    hw_write(c, 0x1f8010e4, 4, 4); hw_write(c, 0x1f8010e8, 4, 0x11000002); advance(24);
    assert(mem_read32(c, 0x200c) == 0x2008 && mem_read32(c, 0x2000) == 0xffffff);
    hw_write(c, 0x1f80104a, 2, 0x1003);
    const unsigned tx[] = {1, 0x42, 0, 0, 0}, rx[] = {0xff, 0x41, 0x5a, 0xff, 0xff};
    for (unsigned i = 0; i < 5; ++i) {
        hw_write(c, 0x1f801040, 1, tx[i]); advance(128);
        assert(hw_read(c, 0x1f801044, 4) & 2); assert(hw_read(c, 0x1f801040, 1) == rx[i]);
        advance(32); assert(!(hw_read(c, 0x1f801044, 4) & 0x80));
    }
    hw_write(c, 0x1f80104a, 2, 0); hw_write(c, 0x1f80104a, 2, 0x1003);
    c->hw->i_stat = 0; hw_write(c, 0x1f801040, 1, 0x81); advance(128);
    assert(hw_read(c, 0x1f801040, 1) == 0xff && !(c->hw->i_stat & 0x80));
}
static void scheduler(void) {
    advance(HW_CPU_HZ);
    assert(c->hw->vblanks == 60);
#ifdef FF8_STATS
    assert(c->hw->event_calls[HW_VIDEO] >= 119 && c->hw->event_calls[HW_VIDEO] <= 120);
    assert(c->hw->event_calls[HW_TIMER] == 0);
    assert(c->hw->event_calls[HW_DMA] == 0 && c->hw->event_calls[HW_CD] == 0);
    assert(c->hw->event_calls[HW_SPU] < 300);
#endif
    /* Counter flags and resets remain observable without scheduled IRQs. */
    hw_write(c, 0x1f801108, 2, 10); hw_write(c, 0x1f801104, 2, 8);
    advance(37);
    assert(hw_read(c, 0x1f801100, 2) == 7);
    assert(hw_read(c, 0x1f801104, 2) & 0x800);
    assert(!(hw_read(c, 0x1f801104, 2) & 0x1800));
    hw_write(c, 0x1f801124, 2, 0);
    advance(200000);
    assert(hw_read(c, 0x1f801120, 2) == (200000 & 0xffff));
    assert(hw_read(c, 0x1f801124, 2) & 0x1800);
    hw_write(c, 0x1f801104, 2, 0x100);
    advance(100);
    hw_write(c, 0x1f801814, 4, 0x08000003);
    assert(hw_read(c, 0x1f801100, 2) == 15);
}

static void dma_irq_deadline(void) {
    for (unsigned i = 0; i < 32; ++i) mem_write32(c, 0x10000 + i * 4, i + 1);
    hw_write(c, 0x1f801da4, 2, 0x201); hw_write(c, 0x1f801da6, 2, 0x200);
    hw_write(c, 0x1f801daa, 2, 0xc060);
    hw_write(c, 0x1f8010f0, 4, 8u << 16);
    hw_write(c, 0x1f8010c0, 4, 0x10000); hw_write(c, 0x1f8010c4, 4, 0x10020);
    hw_write(c, 0x1f8010c8, 4, 0x01000201);
    advance(111); assert(!(c->hw->i_stat & 0x200));
    assert(hw_read(c, 0x1f8010c0, 4) == 0x10008);
    advance(1); assert(c->hw->i_stat & 0x200);
    advance(927); assert(!c->hw->dma[4].completions);
    advance(1); assert(c->hw->dma[4].completions == 1);
    assert(spu_ram(c->hw->spu)[0x1000] == 1 && spu_ram(c->hw->spu)[0x107c] == 32);
}

static void masked_deadlines(void) {
    static const code_span_t span = {0x80010000, 128};
    static const code_region_t region = {&span, 1, 0, 16, NULL};
    unsigned i;
    c->instruction_limit = c->stall_limit = 0;
    c_set_sr(c, 0x401);
    c->recent_region = &region; c->recent_epoch = c->code_epoch; c->recent_valid = 1;
    for (i = 0; i < HW_EVENTS; ++i) c->hw->deadline[i] = HW_NEVER;
    c->hw->deadline[HW_SPU] = c->next_event = 8;
    /* Sampling with I_MASK.SPU clear cannot interrupt this compiled block. */
    assert(c_block(c, 0x80010000, 0, &region) == 1);
    assert(c->interp_instructions == 0);
    c->cycles = spu_next_event(c->hw->spu) - 8;
    c->hw->deadline[HW_SPU] = c->next_event = c->cycles + 8;
    c->hw->i_mask = 0x200;
    assert(!hw_interrupt_window(c, 16));
    spu_write(c->hw->spu, 0x1aa, 0x8040);
    assert(hw_interrupt_window(c, 16));
    {
        static const uint8_t windows[32] = {[0] = 255, [5] = 1};
        static const code_region_t tight = {&span, 1, 0, 16, windows};
        c->recent_region = &tight;
        /* A long path elsewhere in the region must not evict a short entry,
         * or interpret an external target before its own compiled entry. */
        assert(c_block(c, 0x80010014, 0, &tight) == 1);
        assert(c_block(c, 0x80020000, 0, &tight) == 1);
        assert(c->interp_instructions == 0);
        c->recent_region = &region;
    }
    spu_write(c->hw->spu, 0x1aa, 0);
    c->hw->i_mask = 0;
    c->hw->spu_dma.remaining = 1;
    assert(hw_interrupt_window(c, 16));
    c->hw->spu_dma.remaining = 0;
    for (i = 0; i < HW_EVENTS; ++i) {
        if (i == HW_VIDEO || i == HW_SPU) continue;
        c->hw->deadline[i] = c->cycles + 32;
        assert(hw_interrupt_window(c, 16));
        c->hw->deadline[i] = c->cycles + 33;
        assert(!hw_interrupt_window(c, 16));
        c->hw->deadline[i] = HW_NEVER;
    }
    c->hw->i_stat = c->hw->i_mask = 1;
    assert(hw_interrupt_window(c, 16));
    c->hw->i_stat = c->hw->i_mask = 0;
    c->hw->deadline[HW_VIDEO] = c->cycles + 8; c->hw->video_edge = 0;
    assert(!hw_interrupt_window(c, 16));
    c->hw->i_mask = 0x30;
    assert(!hw_interrupt_window(c, 16));
    c->hw->timers[0].mode = c->hw->timers[1].mode = 0x50;
    spu_write(c->hw->spu, 0x1aa, 0x8040);
    for (i = 0x10; i <= 0x200; i <<= 1) {
        c->hw->i_mask = i;
        assert(hw_interrupt_window(c, 16) == !!(i & 0x230));
    }
    c->hw->i_mask = 0;
    for (i = 0; i < 2; ++i) {
        uint64_t vblank = i ? (uint64_t)HW_CPU_HZ * 288 / (50 * 314) :
                             (uint64_t)HW_CPU_HZ * 1001 * 240 / (60000 * 263);
        c->hw->video_pal = i;
        c->cycles = vblank - 33;
        assert(!hw_interrupt_window(c, 16));
        ++c->cycles;
        assert(hw_interrupt_window(c, 16));
    }
}

static void pending_sr(void) {
    c->instruction_limit = 0;
    c_set_sr(c, 0);
    hw_write(c, 0x1f801074, 4, 1); hw_irq_raise(c, 0); hw_reschedule(c);
    assert(c->next_event == c->hw->next_device && c->next_event > c->cycles + 1);
    c_set_sr(c, 0x401); assert(c->next_event == c->cycles + 1);
    c_set_sr(c, 0x404); assert(c->next_event == c->hw->next_device);
    mem_write32(c, 0x10000, 0x42000010); c->pc = 0x80010000;
    interp_step(c);
    assert(!c->halted && c->hw->irq_delivered[0] == 1);
}

static void stats_off(void) {
    uint64_t epoch;
    hw_write(c, 0x1f801814, 4, 0); hw_read(c, 0x1f801814, 4);
    epoch = c->mmio_epoch;
    for (unsigned i = 0; i < 10; ++i) {
        hw_write(c, 0x1f801814, 4, 0); hw_read(c, 0x1f801814, 4);
        assert(c->mmio_epoch == epoch);
    }
    assert(c->mmio_count == 0);
    hw_write(c, 0x1f801074, 2, 1);
    assert(c->mmio_epoch > epoch);
}

int main(int argc, char **argv) {
    assert(argc >= 2); c = malloc(sizeof(*c)); assert(c); cpu_init(c); hw_get(c);
    c->stop_armed = 1;
    if (setjmp(c->stop)) { fprintf(stderr, "%s\n", c->stop_reason); return 1; }
    if (!strcmp(argv[1], "stats_off")) stats_off();
    else if (!strcmp(argv[1], "pending_sr")) pending_sr();
    else if (!strcmp(argv[1], "masked_deadlines")) masked_deadlines();
    else if (!strcmp(argv[1], "scheduler")) scheduler();
    else if (!strcmp(argv[1], "dma_irq_deadline")) dma_irq_deadline();
    else if (!strcmp(argv[1], "cd")) cd_commands(argv[2]);
    else if (!strcmp(argv[1], "dma")) cd_dma(argv[2]);
    else if (!strcmp(argv[1], "cd_status")) cd_status(argv[2]);
    else if (!strcmp(argv[1], "no_disc")) no_disc();
    else if (!strcmp(argv[1], "disc_swap")) disc_swap(argv[2], argv[3]);
    else if (!strcmp(argv[1], "gpu_dma")) gpu_dma();
    else if (!strcmp(argv[1], "events")) events();
    else if (!strcmp(argv[1], "kernel_ram_chains")) kernel_ram_chains();
    else if (!strcmp(argv[1], "kernel_ram_threads")) kernel_ram_threads();
    else if (!strcmp(argv[1], "kernel_ram_threads_syscall")) { thread_syscall = 1; kernel_ram_threads(); }
    else if (!strcmp(argv[1], "kernel_ram_threads_branch")) { thread_branch = 1; kernel_ram_threads(); }
    else if (!strcmp(argv[1], "timers")) timers();
    else if (!strcmp(argv[1], "pal_timing")) pal_timing();
    else if (!strcmp(argv[1], "timer_modes")) timer_modes();
    else if (!strcmp(argv[1], "wait_event")) wait_event();
    else if (!strcmp(argv[1], "bios_pad")) bios_pad();
    else if (!strcmp(argv[1], "misc")) misc();
    else return 2;
    hw_destroy(c); free(c); return 0;
}
