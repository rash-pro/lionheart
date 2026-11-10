#include "hw.h"
#include "sio.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include "native/support/file_size_limit.h"

static cpu_t *c;
static void advance(uint64_t cycles) { c->cycles += cycles; hw_event(c); }
static void reg(unsigned offset, unsigned value) { hw_write(c, 0x1f801040 + offset, offset ? 2 : 1, value); }
static unsigned status(void) { return hw_read(c, 0x1f801044, 4); }
static void select_port(unsigned port) {
    reg(10, 0); reg(8, 0xd); reg(14, 136); reg(10, 0x1003 | (port << 13));
}
static unsigned byte(unsigned tx, int expect_ack) {
    unsigned rx;
    reg(10, c->hw->sio.control | 0x10); c->hw->i_stat &= ~0x80u;
    reg(0, tx); assert(!(status() & 4));
    advance(1088); assert(status() & 2);
    rx = hw_read(c, 0x1f801040, 1);
    assert(!(status() & 2));
    if (expect_ack) {
        assert(c->hw->sio.ack_due != HW_NEVER);
        advance(c->hw->sio.ack_due - c->cycles);
        assert((status() & 0x280) == 0x280 && (c->hw->i_stat & 0x80));
        advance(96); assert(!(status() & 0x80));
    } else { advance(500); assert(!(status() & 0x280)); }
    return rx;
}
static void pad(void) {
    select_port(0); sio_set_buttons(c, 0, PAD_START | PAD_CROSS);
    assert(byte(1, 1) == 0xff); assert(byte(0x42, 1) == 0x41);
    assert(byte(0, 1) == 0x5a); assert(byte(0, 1) == 0xf7); assert(byte(0, 0) == 0xbf);
    assert(byte(1, 0) == 0xff); /* /CS must cycle after the last byte. */
    select_port(1); sio_set_buttons(c, 1, PAD_UP | PAD_TRIANGLE);
    assert(byte(1, 1) == 0xff); assert(byte(0x42, 1) == 0x41);
    assert(byte(0, 1) == 0x5a); assert(byte(0, 1) == 0xef); assert(byte(0, 0) == 0xef);
    select_port(0); assert(byte(1, 1) == 0xff); assert(byte(0x43, 0) == 0xff);
    select_port(1); sio_set_pad(c, 1, 0, 0); assert(byte(1, 0) == 0xff);
    select_port(0); assert(byte(0x81, 0) == 0xff); assert(byte(0x52, 0) == 0xff);
    select_port(0); assert(byte(2, 0) == 0xff); assert(byte(0x42, 0) == 0xff); /* No multitap. */
}
static void timing(void) {
    select_port(0); reg(0, 1);
    assert((status() & 7) == 1);
    advance(1087); assert(!(status() & 0x286));
    advance(1); assert((status() & 7) == 7 && !(status() & 0x280));
    advance(337); assert(!(status() & 0x280));
    advance(1); assert((status() & 0x280) == 0x280);
    advance(95); assert(status() & 0x80);
    advance(1); assert(!(status() & 0x80) && (status() & 0x200));
    reg(10, 0x1013); assert(!(status() & 0x200));
    assert(c->hw->i_stat & 0x80); /* JOY acknowledgement does not acknowledge I_STAT. */
    reg(10, 0x40); assert(status() == 5 && !c->hw->sio.mode && !c->hw->sio.control && !c->hw->sio.baud);
    select_port(0); reg(8, 0xe); reg(0, 1); advance(1088 * 16 - 1); assert(!(status() & 2));
    advance(1); assert(status() & 2);
    reg(10, 0x40); select_port(0); reg(0, 1); advance(500); reg(10, 0); advance(2000);
    assert(!(status() & 0x282));
    reg(10, 0x40); reg(8, 0xd); reg(14, 136); reg(10, 2); reg(0, 1); advance(2000);
    assert(!(status() & 2)); reg(10, 3); advance(1088); assert(status() & 2);
}
static void fifo(void) {
    select_port(0); reg(10, 0xb03); /* RX interrupt at 8 bytes; no ACK interrupt. */
    for (unsigned i = 0; i < 9; ++i) {
        reg(0, i == 0 ? 1 : i == 1 ? 0x42 : 0); advance(2000);
        assert(!!(status() & 0x200) == (i >= 7));
    }
    const unsigned expected[] = {0xff, 0x41, 0x5a, 0xff, 0xff, 0xff, 0xff, 0xff};
    for (unsigned i = 0; i < 8; ++i) assert(hw_read(c, 0x1f801040, 1) == expected[i]);
    assert(!(status() & 2));
    reg(10, 0x40); select_port(0); reg(0, 1); reg(0, 0x42); assert(!(status() & 1));
    advance(1088); assert(status() & 1); assert(!(status() & 4));
    assert(hw_read(c, 0x1f801040, 1) == 0xff); advance(1088);
    assert(hw_read(c, 0x1f801040, 1) == 0x41 && (status() & 4));
}
static void dualshock(void) {
    sio_set_pad(c, 0, 1, 1); select_port(0);
    const unsigned enter[] = {1, 0x43, 0, 1, 0};
    const unsigned normal[] = {0xff, 0x41, 0x5a, 0xff, 0xff};
    for (unsigned i = 0; i < 5; ++i) assert(byte(enter[i], i < 4) == normal[i]);
    select_port(0);
    const unsigned info[] = {0xff, 0xf3, 0x5a, 1, 2, 0, 2, 1, 0};
    for (unsigned i = 0; i < 9; ++i) assert(byte(i == 0 ? 1 : i == 1 ? 0x45 : 0, i < 8) == info[i]);
    select_port(0);
    for (unsigned i = 0; i < 9; ++i) byte(i == 0 ? 1 : i == 1 ? 0x43 : 0, i < 8);
    select_port(0); assert(byte(1, 1) == 0xff); assert(byte(0x42, 1) == 0x41);
}
static void prefix(unsigned port, unsigned command, unsigned sector, unsigned flag) {
    select_port(port); assert(byte(0x81, 1) == 0xff); assert(byte(command, 1) == flag);
    assert(byte(0, 1) == 0x5a); assert(byte(0, 1) == 0x5d);
    if (command != 0x53) { assert(byte(sector >> 8, 1) == 0); assert(byte(sector & 255, 1) == (sector >> 8)); }
}
static unsigned write_sector(unsigned sector, unsigned flag, int bad_checksum) {
    unsigned checksum = (sector >> 8) ^ (sector & 255), previous = sector & 255;
    prefix(0, 0x57, sector, flag);
    for (unsigned i = 0; i < 128; ++i) { assert(byte(i ^ 0x5a, 1) == previous); previous = i ^ 0x5a; checksum ^= previous; }
    assert(byte(checksum ^ bad_checksum, 1) == previous);
    assert(byte(0, 1) == 0x5c); assert(byte(0, 1) == 0x5d); return byte(0, 0);
}
static void card(const char *path) {
    char error[256];
    assert(sio_open_card(c, 0, path, error, sizeof(error)));
    prefix(0, 0x53, 0, 8);
    const unsigned id[] = {0x5c, 0x5d, 4, 0, 0, 0x80};
    for (unsigned i = 0; i < 6; ++i) assert(byte(0, i < 5) == id[i]);
    prefix(0, 0x52, 0, 8);
    reg(10, 0x1013); reg(0, 0); advance(1088);
    assert(hw_read(c, 0x1f801040, 1) == 0x5c); advance(31411); assert(!(status() & 0x200));
    advance(1); assert(status() & 0x200); advance(96);
    assert(byte(0, 1) == 0x5d); assert(byte(0, 1) == 0); assert(byte(0, 1) == 0);
    unsigned checksum = 0;
    for (unsigned i = 0; i < 128; ++i) {
        unsigned value = byte(0, 1); checksum ^= value;
        assert(value == (i == 0 ? 'M' : i == 1 ? 'C' : i == 127 ? 0xe : 0));
    }
    assert(byte(0, 1) == checksum); assert(byte(0, 0) == 0x47);
    assert(write_sector(1023, 8, 1) == 0x4e); assert(write_sector(1024, 8, 0) == 0xff);
    {
        FILE *saved = fopen(path, "rb"); assert(saved); assert(!fseek(saved, 1023 * 128, SEEK_SET));
        for (unsigned i = 0; i < 128; ++i) assert(fgetc(saved) == 255);
        fclose(saved);
    }
    assert(write_sector(1023, 8, 0) == 0x47);
    prefix(0, 0x52, 1023, 0); assert(byte(0, 1) == 0x5c); assert(byte(0, 1) == 0x5d);
    assert(byte(0, 1) == 3); assert(byte(0, 1) == 255); checksum = 3 ^ 255;
    for (unsigned i = 0; i < 128; ++i) { assert(byte(0, 1) == (i ^ 0x5a)); checksum ^= i ^ 0x5a; }
    assert(byte(0, 1) == checksum); assert(byte(0, 0) == 0x47);
    prefix(0, 0x52, 1024, 0); assert(byte(0, 1) == 0x5c); assert(byte(0, 1) == 0x5d);
    assert(byte(0, 1) == 255); assert(byte(0, 0) == 255); assert(byte(0, 0) == 255);
    prefix(0, 0x57, 7, 0); for (unsigned i = 0; i < 64; ++i) byte(0, 1); select_port(0); /* Abort no write. */
    assert(sio_cards_ok(c));
    sio_destroy(c); sio_init(c); assert(sio_open_card(c, 1, path, error, sizeof(error)));
    prefix(1, 0x52, 1023, 8); byte(0, 1); byte(0, 1); byte(0, 1); byte(0, 1);
    for (unsigned i = 0; i < 128; ++i) assert(byte(0, 1) == (i ^ 0x5a));
}
static void script(const char *path, int valid) {
    char error[256];
    assert(sio_load_script(c, path, error, sizeof(error)) == valid);
    if (!valid) { assert(strstr(error, "expected increasing")); return; }
    c->hw->vblanks = 9; sio_input_vblank(c); select_port(0); byte(1, 1); byte(0x42, 1); byte(0, 1);
    assert(byte(0, 1) == 255); assert(byte(0, 0) == 255);
    c->hw->vblanks = 10; sio_input_vblank(c); sio_set_buttons(c, 0, PAD_CIRCLE);
    select_port(0); byte(1, 1); byte(0x42, 1); byte(0, 1);
    assert(byte(0, 1) == 0xf7); assert(byte(0, 0) == 0xbf);
    c->hw->vblanks = 20; sio_input_vblank(c); select_port(0); byte(1, 1); byte(0x42, 1); byte(0, 1);
    assert(byte(0, 1) == 255); assert(byte(0, 0) == 255);
}
static void disc_script(const char *path) {
    char error[256]; uint16_t buttons;
    assert(sio_load_script(c, path, error, sizeof(error)));
    c->hw->vblanks = 10; sio_input_vblank(c);
    assert(sio_pad_snapshot(c, 0, &buttons) && buttons == (uint16_t)~PAD_CROSS);
    c->hw->vblanks = 15; sio_input_vblank(c);
    assert(sio_input_disc(c) == 2 && sio_input_disc(c) == 0);
    assert(sio_pad_snapshot(c, 0, &buttons) && buttons == (uint16_t)~PAD_CROSS);
    c->hw->vblanks = 20; sio_input_vblank(c);
    assert(sio_pad_snapshot(c, 0, &buttons) && buttons == 0xffff);
}
int main(int argc, char **argv) {
    assert(argc >= 2); c = malloc(sizeof(*c)); assert(c); cpu_init(c); sio_init(c);
    if (!strcmp(argv[1], "pad")) pad();
    else if (!strcmp(argv[1], "timing")) timing();
    else if (!strcmp(argv[1], "fifo")) fifo();
    else if (!strcmp(argv[1], "dualshock")) dualshock();
    else if (!strcmp(argv[1], "card")) card(argv[2]);
    else if (!strcmp(argv[1], "script")) script(argv[2], 1);
    else if (!strcmp(argv[1], "disc_script")) disc_script(argv[2]);
    else if (!strcmp(argv[1], "invalid_script")) script(argv[2], 0);
    else if (!strcmp(argv[1], "invalid_card")) {
        char error[256]; assert(!sio_open_card(c, 0, argv[2], error, sizeof(error)));
    } else if (!strcmp(argv[1], "card_failed")) {
        char error[256];
        assert(sio_open_card(c, 0, argv[2], error, sizeof(error)));
        lh_test_limit_file_size();
        assert(write_sector(1023, 8, 0) == 0x4e); assert(!sio_cards_ok(c));
        select_port(0); assert(byte(0x81, 1) == 255); assert(byte(0x53, 1) == 12);
    } else if (!strcmp(argv[1], "card_existing")) {
        char error[256]; assert(sio_open_card(c, 0, argv[2], error, sizeof(error)));
    } else return 2;
    sio_destroy(c); hw_destroy(c); free(c); return 0;
}
