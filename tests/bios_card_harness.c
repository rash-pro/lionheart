#include "hw.h"
#include "sio.h"
#include <assert.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>

static cpu_t *c;
static uint32_t call(unsigned table, unsigned fn, uint32_t a, uint32_t b, uint32_t n) {
    c->r[9] = fn; c->r[4] = a; c->r[5] = b; c->r[6] = n;
    bios_call(c, table); return c->r[2];
}
static uint32_t event(uint32_t class_id, uint32_t spec, uint32_t mode, uint32_t handler) {
    uint32_t handle;
    c->r[7] = handler; handle = call(1, 8, class_id, spec, mode);
    assert(handle != ~0u); assert(call(1, 0xc, handle, 0, 0)); return handle;
}
static int ready(uint32_t handle) { return call(1, 0xb, handle, 0, 0); }
static void attach(unsigned slot, const char *path) {
    char error[256]; assert(sio_open_card(c, slot, path, error, sizeof(error)));
}
static void start(void) { call(1, 0x4a, 1, 0, 0); call(1, 0x4b, 0, 0, 0); }
static void missing(void) {
    uint32_t hw = event(0xf0000011, 0x100, 0x2000, 0);
    uint32_t sw = event(0xf4000001, 0x100, 0x2000, 0);
    assert(call(1, 0x5d, 0, 0, 0) == 1);
    assert(!call(1, 0x4f, 0, 0, 0x10000));
    start(); assert(call(0, 0xab, 0x10, 0, 0));
    assert(call(1, 0x5c, 1, 0, 0) == 8);
    assert(!ready(hw) && !ready(sw));
    c->cycles += HW_CPU_HZ / 30 - 1; hw_event(c);
    assert(!ready(hw));
    assert(call(1, 0x5d, 1, 0, 0) == 0x11);
    assert(ready(hw) && ready(sw)); assert(!ready(hw));
    assert(call(1, 0x58, 0, 0, 0) == 0x10);
    assert(!call(1, 0x4f, 0x11, 0, 0x10000));
    assert(!call(1, 0x4f, 0, 1024, 0x10000));
    assert(call(1, 0x5c, 3, 0, 0) == 0x11);
}
static void storage(const char *path) {
    unsigned i; uint8_t sector[128];
    uint32_t changed = event(0xf0000011, 0x2000, 0x2000, 0);
    uint32_t done = event(0xf0000011, 4, 0x2000, 0);
    attach(0, path); start();
    assert(call(1, 0x4f, 0, 0, 0x10000));
    assert(!call(1, 0x4d, 0, 0, 0));
    assert(call(1, 0x5d, 0, 0, 0) == 0x21 && ready(changed));
    call(1, 0x50, 0, 0, 0);
    assert(call(1, 0x4f, 0, 0, 0x10000));
    assert(call(1, 0x5c, 0, 0, 0) == 2 && !ready(done));
    assert(call(1, 0x5d, 0, 0, 0) == 1 && ready(done));
    assert(mem_read16(c, 0x10000) == 0x434d);
    assert(memcard_flags(sio_card(c, 0)) & 8);
    assert(call(0, 0xaf, 0, 0, 0)); assert(call(1, 0x5d, 0, 0, 0) == 1);
    assert(!(memcard_flags(sio_card(c, 0)) & 8));
    for (i = 0; i < 128; ++i) mem_write8(c, 0x10000 + i, i ^ 0x5a);
    assert(call(1, 0x4e, 0, 1023, 0x10000));
    for (i = 0; i < 128; ++i) mem_write8(c, 0x10000 + i, 0);
    assert(call(1, 0x5c, 0, 0, 0) == 4);
    assert(call(1, 0x5d, 0, 0, 0) == 1);
    assert(memcard_read_sector(sio_card(c, 0), 1023, sector));
    for (i = 0; i < 128; ++i) assert(sector[i] == (i ^ 0x5a));
    {
        memcard_t *card = sio_card(c, 0);
        unsigned ack, checksum = 3 ^ 0xfe;
        memcard_reset(card);
        for (i = 0; i < 140; ++i) {
            unsigned tx = i == 0 ? 0x81 : i == 1 ? 0x52 : i == 4 ? 3 : i == 5 ? 0xff : 0;
            unsigned rx = memcard_exchange(card, (uint8_t)tx, &ack);
            if (i >= 10 && i < 138) assert(rx == ((i - 10) ^ 0x5a));
        }
        memcard_reset(card);
        for (i = 0; i < 138; ++i) {
            unsigned tx = i == 0 ? 0x81 : i == 1 ? 0x57 : i == 4 ? 3 : i == 5 ? 0xfe :
                          i >= 6 && i < 134 ? (i - 6) ^ 0xa5 : i == 134 ? checksum : 0;
            if (i >= 6 && i < 134) checksum ^= tx;
            unsigned rx = memcard_exchange(card, (uint8_t)tx, &ack);
            if (i == 137) assert(rx == 0x47);
        }
        assert(call(1, 0x4f, 0, 1022, 0x10000)); assert(call(1, 0x5d, 0, 0, 0) == 1);
        for (i = 0; i < 128; ++i) assert(mem_read8(c, 0x10000 + i) == (i ^ 0xa5));
    }
    sio_destroy(c); sio_init(c); attach(1, path);
    call(1, 0x50, 0, 0, 0); assert(call(1, 0x4f, 0x10, 1023, 0x10000));
    assert(call(1, 0x5d, 1, 0, 0) == 1);
    for (i = 0; i < 128; ++i) assert(mem_read8(c, 0x10000 + i) == (i ^ 0x5a));
    assert(call(1, 0x4d, 0x10, 0, 0)); call(1, 0x4c, 0, 0, 0);
    c->cycles += HW_CPU_HZ; hw_event(c);
    assert(call(1, 0x5d, 1, 0, 0) == 1);
    assert(!call(1, 0x4f, 0x10, 0, 0x10000));
}
static void load(const char *path) {
    uint32_t sw = event(0xf4000001, 4, 0x2000, 0);
    attach(0, path); start();
    assert(call(0, 0xac, 0, 0, 0)); assert(!ready(sw));
    assert(call(1, 0x5d, 0, 0, 0) == 1 && ready(sw));
    assert(call(0, 0xab, 0, 0, 0)); assert(call(1, 0x5d, 0, 0, 0) == 1 && ready(sw));
    assert(call(0, 0xad, 1, 0, 0) == 0); assert(call(0, 0xad, 0, 0, 0) == 1);
    call(0, 0x55, 0, 0, 0);
    assert(call(1, 0x5c, 0, 0, 0) == 1 && call(1, 0x5c, 1, 0, 0) == 0x11);
}
static void unformatted(const char *path) {
    uint32_t fresh = event(0xf4000001, 0x2000, 0x2000, 0);
    uint32_t done = event(0xf4000001, 4, 0x2000, 0);
    attach(0, path); start();
    assert(call(0, 0xac, 0, 0, 0));
    assert(call(1, 0x5d, 0, 0, 0) == 0x21 && ready(fresh));
    assert(call(0, 0xad, 1, 0, 0) == 0);
    assert(call(0, 0xac, 0, 0, 0));
    assert(call(1, 0x5d, 0, 0, 0) == 1 && ready(done));
    assert(call(1, 0x4f, 0, 0, 0x10000));
    assert(call(1, 0x5d, 0, 0, 0) == 1 && mem_read16(c, 0x10000) == 0x434d);
}
static void callback(cpu_t *cpu) {
    mem_write32(cpu, 0x6000, mem_read32(cpu, 0x6000) + 1);
    cpu->r[8] = 0xdeadbeef; cpu->hi = 45; cpu->lo = 67; cpu->pc = cpu->r[31];
}
static const entry_t callbacks[] = {{0x80010000, callback}};
static void callbacks_test(void) {
    uint32_t saved[32], hi, lo, pc;
    c->entries = callbacks; c->entry_count = 1;
    event(0xf0000011, 0x100, 0x1000, 0x80010000); start();
    assert(call(1, 0x4d, 0, 0, 0));
    memcpy(saved, c->r, sizeof(saved)); hi = c->hi; lo = c->lo; pc = c->pc;
    c_set_sr(c, c->cop0[12] & ~1u);
    c->cycles += HW_CPU_HZ; hw_event(c); assert(!mem_read32(c, 0x6000));
    c_set_sr(c, c->cop0[12] | 0x401); hw_event(c);
    assert(mem_read32(c, 0x6000) == 1 && !memcmp(saved, c->r, sizeof(saved)));
    assert(c->hi == hi && c->lo == lo && c->pc == pc);
    hw_event(c); assert(mem_read32(c, 0x6000) == 1);
}
static void failure(const char *path) {
    struct rlimit limit = {0, 0};
    uint32_t err = event(0xf0000011, 0x8000, 0x2000, 0);
    attach(0, path); start(); call(1, 0x50, 0, 0, 0);
    signal(SIGXFSZ, SIG_IGN); assert(!setrlimit(RLIMIT_FSIZE, &limit));
    assert(call(1, 0x4e, 0, 100, 0x10000));
    assert(call(1, 0x5d, 0, 0, 0) == 0x21 && ready(err));
    assert(!sio_cards_ok(c));
}
static void pad(void) {
    call(1, 0x12, 0x10000, 4, 0); call(1, 0x13, 0, 0, 0);
    sio_set_buttons(c, 0, PAD_CROSS | PAD_START); bios_pad_vblank(c);
    assert(mem_read32(c, 0x10000) == 0xbff74100);
    sio_set_pad(c, 0, 0, 0); bios_pad_vblank(c); assert(mem_read8(c, 0x10000) == 0xff);
}
static uint32_t text(const char *s) {
    unsigned i;
    for (i = 0; i <= strlen(s); ++i) mem_write8(c, 0x10000 + i, (uint8_t)s[i]);
    return 0x10000;
}
static uint32_t card_device, original_firstfile;
static unsigned firstfile_hooks;
static void firstfile_hook(cpu_t *cpu) {
    ++firstfile_hooks;
    assert(cpu->r[6] == 0x11000);
    if (!mem_read32(cpu, cpu->r[4])) mem_write32(cpu, cpu->r[4], 1);
    mem_write32(cpu, card_device + 0x34, original_firstfile);
    cpu->r[2] = bios_guest_call(cpu, original_firstfile);
    cpu->pc = cpu->r[31];
}
static void device_directory(const char *path) {
    static const entry_t hooks[] = {{0x80018000, firstfile_hook}};
    uint32_t fd, name;
    card_device = mem_read32(c, 0x150);
    assert(card_device && mem_read32(c, 0x154) == 10 * 0x50);
    name = mem_read32(c, card_device);
    assert(mem_read8(c, name) == 'b' && mem_read8(c, name + 1) == 'u' && !mem_read8(c, name + 2));
    assert(mem_read32(c, card_device + 4) == 0x14 && mem_read32(c, card_device + 8) == 128);
    original_firstfile = mem_read32(c, card_device + 0x34); assert(original_firstfile);
    attach(0, path); start();
    fd = call(1, 0x32, text("bu00:SAVE"), 0x10203, 0); assert(fd == 2);
    assert(call(1, 0x36, fd, 0, 0) == fd);
    c->entries = hooks; c->entry_count = 1;
    mem_write32(c, card_device + 0x34, 0x80018000);
    assert(call(1, 0x42, text("bu00:*"), 0x11000, 0) == 0x11000);
    assert(firstfile_hooks == 1 && mem_read32(c, card_device + 0x34) == original_firstfile);
    assert(mem_read8(c, 0x11000) == 'S' && mem_read32(c, 0x11018) == 8192);
    assert(call(1, 0x42, text("bu00:*"), 0x11000, 0) == 0x11000 && firstfile_hooks == 1);
}
static void filesystem(const char *path) {
    unsigned i, fd; uint8_t data[128];
    attach(0, path); start();
    assert(call(1, 0x42, text("bu00:*"), 0x11000, 0) == 0);
    fd = call(0, 0, text("bu00:SAVE"), 0x20203, 0); assert(fd == 2);
    assert(call(0, 0, text("bu00:SAVE"), 0x10203, 0) == ~0u);
    assert(call(1, 0x54, 0, 0, 0) == 0x11);
    for (i = 0; i < 8320; ++i) mem_write8(c, 0x12000 + i, (uint8_t)(i ^ (i >> 8)));
    assert(call(0, 3, fd, 0x12000, 8320) == 8320);
    assert(call(0, 1, fd, 0, 0) == 0);
    assert(call(0, 2, fd, 0x18000, 8320) == 8320);
    for (i = 0; i < 8320; ++i) assert(mem_read8(c, 0x18000 + i) == (uint8_t)(i ^ (i >> 8)));
    assert(call(0, 1, fd, 0, 2) == 16384); assert(call(0, 2, fd, 0x18000, 128) == 0);
    assert(call(0, 1, fd, 1, 0) == 1); assert(call(0, 2, fd, 0x18000, 128) == ~0u);
    assert(call(1, 0x42, text("bu00:SA*"), 0x11000, 0) == 0x11000);
    assert(mem_read32(c, 0x11018) == 16384 && mem_read32(c, 0x11020) == 64);
    assert(call(1, 0x43, 0x11000, 0, 0) == 0);
    assert(!call(1, 0x45, text("bu00:SAVE"), 0, 0)); assert(call(0, 4, fd, 0, 0) == fd);
    for (i = 2; i < 16; ++i) assert(call(1, 0x32, text("bu00:SAVE"), 1, 0) == i);
    assert(call(0, 0, text("bu00:SAVE"), 1, 0) == ~0u && call(1, 0x54, 0, 0, 0) == 0x18);
    for (i = 2; i < 16; ++i) assert(call(1, 0x36, i, 0, 0) == i);
    assert(call(1, 0x45, text("bu00:SAVE"), 0, 0));
    assert(!call(1, 0x42, text("bu00:*"), 0x11000, 0));
    assert(call(0, 0, text("bu00:A"), 0x10203, 0) == 2); call(0, 4, 2, 0, 0);
    assert(call(0, 0, text("bu00:B"), 0x10203, 0) == 2); call(0, 4, 2, 0, 0);
    assert(call(1, 0x45, text("bu00:A"), 0, 0));
    fd = call(0, 0, text("bu00:C"), 0x20203, 0); assert(fd == 2);
    assert(memcard_read_sector(sio_card(c, 0), 1, data) && data[8] == 2 && data[9] == 0);
    assert(call(1, 0x35, fd, 0x12000, 8320) == 8320); call(0, 4, fd, 0, 0);
    sio_destroy(c); sio_init(c); attach(0, path);
    fd = call(1, 0x32, text("bu00:C"), 3, 0); assert(fd == 2);
    assert(call(1, 0x34, fd, 0x18000, 8320) == 8320);
    for (i = 0; i < 8320; ++i) assert(mem_read8(c, 0x18000 + i) == (uint8_t)(i ^ (i >> 8)));
    call(0, 4, fd, 0, 0);
    assert(call(1, 0x41, text("bu00:"), 0, 0));
    assert(!call(1, 0x42, text("bu00:*"), 0x11000, 0));
    assert(call(0, 0, text("bu10:MISSING"), 1, 0) == ~0u);
}
static void filesystem_async(const char *path, int failed) {
    uint32_t fd, sw, finished;
    struct rlimit limit = {0, 0};
    attach(0, path); start(); fd = call(0, 0, text("bu00:ASYNC"), 0x18203, 0); assert(fd == 2);
    sw = event(0xf4000001, failed ? 0x8000 : 4, 0x2000, 0);
    finished = event(fd, 4, 0x2000, 0);
    if (failed) { signal(SIGXFSZ, SIG_IGN); assert(!setrlimit(RLIMIT_FSIZE, &limit)); }
    assert(call(0, 3, fd, 0x12000, 256) == 0);
    assert(!ready(sw) && !ready(finished));
    assert(call(0, 1, fd, 0, 0) == ~0u);
    assert(call(1, 0x5d, 0, 0, 0) == (failed ? 0x21u : 1u)); assert(ready(sw));
    assert(ready(finished) == !failed);
    assert(call(1, 0x55, fd, 0, 0) == (failed ? 5u : 0u));
    if (!failed) { assert(call(0, 1, fd, 0, 1) == 256); assert(sio_cards_ok(c)); }
    else assert(!sio_cards_ok(c));
}
static void filesystem_corrupt(const char *path) {
    uint8_t data[128]; unsigned i;
    attach(0, path); start(); assert(call(0, 0, text("bu00:BAD"), 0xf0203, 0) == 2);
    call(0, 4, 2, 0, 0);
    assert(memcard_read_sector(sio_card(c, 0), 15, data)); data[8] = data[9] = 0;
    data[127] = 0; for (i = 0; i < 127; ++i) data[127] ^= data[i];
    assert(memcard_write_sector(sio_card(c, 0), 15, data));
    assert(call(0, 0, text("bu00:BAD"), 3, 0) == ~0u && call(1, 0x54, 0, 0, 0) == 5);
}
int main(int argc, char **argv) {
    assert(argc >= 2); c = malloc(sizeof(*c)); assert(c); cpu_init(c); sio_init(c);
    c_set_sr(c, c->cop0[12] | 0x401); c->stop_armed = 1;
    if (setjmp(c->stop)) { fprintf(stderr, "%s\n", c->stop_reason); return 1; }
    if (!strcmp(argv[1], "device_directory")) device_directory(argv[2]);
    else if (!strcmp(argv[1], "missing")) missing();
    else if (!strcmp(argv[1], "storage")) storage(argv[2]);
    else if (!strcmp(argv[1], "load")) load(argv[2]);
    else if (!strcmp(argv[1], "unformatted")) unformatted(argv[2]);
    else if (!strcmp(argv[1], "callbacks")) callbacks_test();
    else if (!strcmp(argv[1], "failure")) failure(argv[2]);
    else if (!strcmp(argv[1], "pad")) pad();
    else if (!strcmp(argv[1], "filesystem")) filesystem(argv[2]);
    else if (!strcmp(argv[1], "filesystem_async")) filesystem_async(argv[2], 0);
    else if (!strcmp(argv[1], "filesystem_failure")) filesystem_async(argv[2], 1);
    else if (!strcmp(argv[1], "filesystem_corrupt")) filesystem_corrupt(argv[2]);
    else return 2;
    sio_destroy(c); hw_destroy(c); free(c); return 0;
}
