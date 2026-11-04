#include "ff8_state.h"
#include "ff8_ram_layout.h"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

static unsigned u16(const cpu_t *c, unsigned at) {
    return c->ram[at] | ((unsigned)c->ram[at + 1] << 8);
}
static uint32_t u32(const cpu_t *c, unsigned at) {
    return u16(c, at) | ((uint32_t)u16(c, at + 2) << 16);
}
typedef struct { char *out; size_t left; int failed; } json_t;
static void append(json_t *j, const char *format, ...) {
    va_list args;
    int n;
    if (j->failed) return;
    va_start(args, format); n = vsnprintf(j->out, j->left, format, args); va_end(args);
    if (n < 0 || (size_t)n >= j->left) { j->failed = 1; return; }
    j->out += n; j->left -= (size_t)n;
}
static const char *mode_name(unsigned raw) {
    switch (raw) {
    case 1: return "field";
    case 2: return "world";
    case 3: return "battle";
    case 6: return "menu";
    case 8: return "title";
    default: return "unknown";
    }
}
static const char *const characters[] = {"Squall", "Zell", "Irvine", "Quistis", "Rinoa", "Selphie", "Seifer", "Edea"};
static const char *const gfs[] = {"Quezacotl", "Shiva", "Ifrit", "Siren", "Brothers", "Diablos", "Carbuncle", "Leviathan",
    "Pandemona", "Cerberus", "Alexander", "Doomtrain", "Bahamut", "Cactuar", "Tonberry", "Eden"};
/* Disc 1's active field lookup and file table are in the field overlay, not the
 * save's location-label index. Catalog field_ids are not runtime IDs. */
#include "ff8_field_names.h"
static const char *field_name(const cpu_t *c, unsigned disc, unsigned mode, unsigned id) {
    unsigned local, lba, i;
    if (disc != 1 || !(mode == 1 || (mode == 6 && u16(c, FF8_SAVEMAP + 0xd40) == 1)) || id >= 910) return NULL;
    local = u16(c, 0xc2568 + id * 2);
    if (local >= 309) return NULL;
    lba = u32(c, 0xc0908 + local * 24);
    for (i = 0; i < sizeof(ff8_disc1_fields) / sizeof(ff8_disc1_fields[0]); ++i)
        if (ff8_disc1_fields[i].lba == lba) return ff8_disc1_fields[i].name;
    return NULL;
}

/* Metadata fingerprint of the disc-check/boot module (top-table entry 1),
 * invariant across the four USA discs. Its code is resident after soft reset
 * while the global mode and save header may still describe the old field. */
static int boot_module(const cpu_t *c) {
    uint64_t hash = UINT64_C(14695981039346656037);
    unsigned at;
    for (at = 0x98180; at < 0x98280; ++at) hash = (hash ^ c->ram[at]) * UINT64_C(1099511628211);
    return hash == UINT64_C(0x8a8acc91225252a6);
}

void host_ff8_state_json(const cpu_t *c, char *out, size_t capacity) {
    json_t j = {out, capacity, 0};
    unsigned s = FF8_SAVEMAP, mode = u16(c, FF8_MODE), disc = c->ram[s + FF8_DISC];
    unsigned i, first = 1, id = u16(c, FF8_FIELD_ID), total_kills = 0;
    int live_mode = mode == 1 || mode == 2 || mode == 3 || mode == 6;
    int initialized = live_mode && u16(c, s + 2) == 0x08ff && disc >= 1 && disc <= 4;
    int boot = initialized && boot_module(c);
    int available = initialized && !boot;
    const char *name = available ? field_name(c, disc, mode, id) : NULL;
    const char *mode_text = boot ? "title" : mode_name(mode);
    append(&j, "{\"game\":\"ff8-usa-slus-00892\",\"available\":%s,\"mode\":{\"value\":\"%s\",\"raw\":%u,\"verified\":%s}",
        available ? "true" : "false", mode_text, mode, !boot && strcmp(mode_text, "unknown") ? "true" : "false");
    if (!available) { append(&j, "}"); goto done; }
    append(&j, ",\"field\":{\"id\":%u,\"verified\":%s,\"name\":", id, (mode == 1 || name) ? "true" : "false");
    if (name) append(&j, "\"%s\"", name); else append(&j, "null");
    append(&j, ",\"name_verified\":%s},\"disc\":{\"value\":%u,\"verified\":false}", name ? "true" : "false", disc);
    append(&j, ",\"saved_play_time_seconds\":{\"value\":%u,\"verified\":true},\"play_time_seconds\":{\"value\":%u,\"verified\":true}", u32(c, s + 0x10), u32(c, s + FF8_PLAY_TIME));
    append(&j, ",\"gil\":{\"value\":%u,\"verified\":true},\"party\":[", u32(c, s + FF8_GIL));
    for (i = 0; i < 3; ++i) {
        unsigned member = c->ram[s + FF8_PARTY + i];
        if (member == 255) continue;
        append(&j, "%s%u", first ? "" : ",", member); first = 0;
    }
    append(&j, "],\"party_verified\":true,\"characters\":[");
    for (i = 0; i < 8; ++i) {
        unsigned a = s + FF8_CHARACTER + i * FF8_CHARACTER_SIZE;
        uint32_t exp = u32(c, a + 4), level = exp / 1000 + 1;
        total_kills += u16(c, a + 0x90);
        if (level > 100) level = 100;
        append(&j, "%s{\"id\":%u,\"name\":\"%s\",\"level\":{\"value\":%u,\"verified\":true},\"hp\":{\"value\":%u,\"verified\":true},\"max_hp\":{\"value\":%u,\"verified\":false},\"experience\":%u,\"kills\":{\"value\":%u,\"verified\":true},\"kos\":{\"value\":%u,\"verified\":false}}",
            i ? "," : "", i, characters[i], level, u16(c, a), u16(c, a + 2), exp, u16(c, a + 0x90), u16(c, a + 0x92));
    }
    append(&j, "],\"total_kills\":{\"value\":%u,\"verified\":true},\"battles_won\":{\"value\":%u,\"verified\":true},\"battles_escaped\":{\"value\":%u,\"verified\":true},\"draw_count\":{\"value\":null,\"verified\":false},\"draw_points\":{\"value\":null,\"verified\":false}", total_kills, u32(c, s + FF8_BATTLES_WON), u16(c, s + 0xce2));
    append(&j, ",\"triple_triad\":{\"wins\":null,\"losses\":null,\"draws\":null,\"verified\":false},\"seed_rank\":{\"value\":null,\"verified\":false},\"gfs\":[");
    for (i = 0; i < 16; ++i) {
        unsigned a = s + FF8_GF + i * FF8_GF_SIZE;
        append(&j, "%s{\"id\":%u,\"name\":\"%s\",\"acquired\":%s,\"verified\":false}", i ? "," : "", i, gfs[i], c->ram[a + 0x11] & 1 ? "true" : "false");
    }
    append(&j, "]}");
done:
    if (j.failed && capacity) snprintf(out, capacity, "{\"error\":\"snapshot too large\"}");
}
