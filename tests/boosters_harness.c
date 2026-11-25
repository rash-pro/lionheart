#include "boosters.h"
#include "ff8_ram_layout.h"
#include "audio.h"
#include "savestate.h"
#include "hw.h"
#include "sio.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint16_t u16(const cpu_t *c, unsigned at) {
    return (uint16_t)(c->ram[at] | ((unsigned)c->ram[at + 1] << 8));
}
static void w16(cpu_t *c, unsigned at, uint16_t v) {
    c->ram[at] = (uint8_t)(v & 0xffu);
    c->ram[at + 1] = (uint8_t)((v >> 8) & 0xffu);
}

static void init_live_ram(cpu_t *c, unsigned mode) {
    memset(c->ram, 0, sizeof(c->ram));
    w16(c, FF8_MODE, (uint16_t)mode);
    w16(c, FF8_SAVEMAP + 2, 0x08ffu);
    c->ram[FF8_SAVEMAP + FF8_DISC] = 1;
    c->ram[FF8_SAVEMAP + FF8_PARTY + 0] = 0;   /* Squall */
    c->ram[FF8_SAVEMAP + FF8_PARTY + 1] = 1;   /* Zell */
    c->ram[FF8_SAVEMAP + FF8_PARTY + 2] = 255; /* empty slot */
}

static void test_boosters_ram(void) {
    cpu_t *c = calloc(1, sizeof(*c));
    host_boosters_t b;
    assert(c);

    /* Default off: RAM untouched, not tainted. */
    init_live_ram(c, 1);
    w16(c, FF8_ENCOUNTER_CONTROL, 0x0003u);
    host_boosters_init(&b, 0, 0, 0);
    assert(!host_boosters_gameplay_active(&b));
    assert(!host_boosters_tainted(&b));
    assert(host_boosters_speed_scale(&b) == 1.0);
    host_boosters_frame(&b, c);
    assert(u16(c, FF8_ENCOUNTER_CONTROL) == 0x0003u);

    /* 3x speed alone does not taint RA. */
    host_boosters_set_speed(&b, 1);
    assert(host_boosters_speed_scale(&b) == 3.0);
    assert(!host_boosters_tainted(&b));
    host_boosters_set_speed(&b, 0);
    assert(host_boosters_speed_scale(&b) == 1.0);

    /* No encounters in field (mode 1) and world map (mode 2), plus clean restore. */
    host_boosters_set_gameplay(&b, 1, 0);
    assert(host_boosters_gameplay_active(&b));
    assert(host_boosters_tainted(&b));
    host_boosters_frame(&b, c);
    assert(u16(c, FF8_ENCOUNTER_CONTROL) == FF8_ENCOUNTER_NONE);
    w16(c, FF8_MODE, 2);
    w16(c, FF8_ENCOUNTER_CONTROL, 0x0001u);
    host_boosters_frame(&b, c);
    assert(u16(c, FF8_ENCOUNTER_CONTROL) == FF8_ENCOUNTER_NONE);

    /* Disabling restores saved value, but session remains permanently tainted. */
    host_boosters_set_gameplay(&b, 0, 0);
    assert(!host_boosters_gameplay_active(&b));
    assert(host_boosters_tainted(&b));
    host_boosters_frame(&b, c);
    assert(u16(c, FF8_ENCOUNTER_CONTROL) == 0x0003u);

    /* Uninitialized save header is never modified. */
    memset(c->ram, 0, sizeof(c->ram));
    w16(c, FF8_MODE, 1);
    w16(c, FF8_ENCOUNTER_CONTROL, 0x0005u);
    host_boosters_set_gameplay(&b, 1, 1);
    host_boosters_frame(&b, c);
    assert(u16(c, FF8_ENCOUNTER_CONTROL) == 0x0005u);

    /* Battle assist: heals active party slots to max HP, fills ATB, sets Aura/Limit bit; skips empty slot 2. */
    init_live_ram(c, 3);
    w16(c, FF8_BATTLE_MAX_HP + 0 * FF8_BATTLE_SLOT_STRIDE, 1200u);
    w16(c, FF8_BATTLE_CUR_HP + 0 * FF8_BATTLE_SLOT_STRIDE, 300u);
    w16(c, FF8_BATTLE_ATB + 0 * FF8_BATTLE_SLOT_STRIDE, 400u);
    w16(c, FF8_BATTLE_MAX_HP + 1 * FF8_BATTLE_SLOT_STRIDE, 950u);
    w16(c, FF8_BATTLE_CUR_HP + 1 * FF8_BATTLE_SLOT_STRIDE, 1u);
    w16(c, FF8_BATTLE_ATB + 1 * FF8_BATTLE_SLOT_STRIDE, 0u);
    /* Slot 2 is empty (party[2] == 255) and must remain untouched. */
    w16(c, FF8_BATTLE_MAX_HP + 2 * FF8_BATTLE_SLOT_STRIDE, 500u);
    w16(c, FF8_BATTLE_CUR_HP + 2 * FF8_BATTLE_SLOT_STRIDE, 100u);
    w16(c, FF8_BATTLE_ATB + 2 * FF8_BATTLE_SLOT_STRIDE, 123u);

    host_boosters_set_gameplay(&b, 0, 1);
    host_boosters_frame(&b, c);
    assert(u16(c, FF8_BATTLE_CUR_HP + 0 * FF8_BATTLE_SLOT_STRIDE) == 1200u);
    assert(u16(c, FF8_BATTLE_ATB + 0 * FF8_BATTLE_SLOT_STRIDE) == FF8_BATTLE_ATB_MAX);
    assert((c->ram[FF8_BATTLE_STATUS_AURA + 0 * FF8_BATTLE_SLOT_STRIDE] & FF8_BATTLE_AURA_BIT) != 0);
    assert((u16(c, FF8_BATTLE_ENTITY_AURA + 0 * FF8_BATTLE_ENTITY_STRIDE) & FF8_BATTLE_AURA_BIT) != 0);
    assert(u16(c, FF8_BATTLE_CUR_HP + 1 * FF8_BATTLE_SLOT_STRIDE) == 950u);
    assert(u16(c, FF8_BATTLE_ATB + 1 * FF8_BATTLE_SLOT_STRIDE) == FF8_BATTLE_ATB_MAX);
    assert((c->ram[FF8_BATTLE_STATUS_AURA + 1 * FF8_BATTLE_SLOT_STRIDE] & FF8_BATTLE_AURA_BIT) != 0);
    assert(u16(c, FF8_BATTLE_CUR_HP + 2 * FF8_BATTLE_SLOT_STRIDE) == 100u);
    assert(u16(c, FF8_BATTLE_ATB + 2 * FF8_BATTLE_SLOT_STRIDE) == 123u);
    assert(c->ram[FF8_BATTLE_STATUS_AURA + 2 * FF8_BATTLE_SLOT_STRIDE] == 0);

    free(c);
}

static void test_savestate_taint_compat(const char *path) {
    cpu_t *c = calloc(1, sizeof(*c));
    cpu_t *loaded;
    savestate_identity_t write_id, read_id;
    char error[256] = {0};
    assert(c);
    cpu_init(c);
    hw_get(c);
    sio_init(c);
    memset(&write_id, 0, sizeof(write_id));
    strcpy(write_id.build, "boosters-test-build");
    write_id.exe = 0x12345678u;
    write_id.modes = 1u | SAVESTATE_MODE_BOOSTERS_TAINTED;
    assert(savestate_write(c, path, &write_id, error, sizeof(error)));

    /* A clean session (without taint bit) can load a tainted savestate and learns the taint bit. */
    memset(&read_id, 0, sizeof(read_id));
    strcpy(read_id.build, "boosters-test-build");
    read_id.exe = 0x12345678u;
    read_id.modes = 1u;
    loaded = savestate_read(c, path, &read_id, NULL, NULL, error, sizeof(error));
    assert(loaded != NULL);
    assert((read_id.modes & SAVESTATE_MODE_BOOSTERS_TAINTED) != 0);
    savestate_dispose(loaded);

    /* A different non-taint mode bit still fails compatibility checks. */
    memset(&read_id, 0, sizeof(read_id));
    strcpy(read_id.build, "boosters-test-build");
    read_id.exe = 0x12345678u;
    read_id.modes = 2u;
    loaded = savestate_read(c, path, &read_id, NULL, NULL, error, sizeof(error));
    assert(loaded == NULL);

    sio_destroy(c);
    hw_destroy(c);
    free(c);
}

static void test_audio_speed_resample(const char *csv_path) {
    assert(host_audio_init(1, 1, NULL));
    assert(host_audio_stats(csv_path));
    host_audio_speed(1.0);
    host_audio_pace(0, 0);
    host_audio_speed(3.0);
    host_audio_pace(0, 0);
    assert(host_audio_shutdown(NULL));
}

int main(int argc, char **argv) {
    if (argc < 3) return 2;
    test_boosters_ram();
    test_savestate_taint_compat(argv[1]);
    test_audio_speed_resample(argv[2]);
    puts("ok");
    return 0;
}
