#include "boosters.h"
#include "ff8_ram_layout.h"
#include <string.h>

static uint16_t read16(const cpu_t *c, unsigned at) {
    return (uint16_t)(c->ram[at] | ((unsigned)c->ram[at + 1] << 8));
}
static void write16(cpu_t *c, unsigned at, uint16_t value) {
    c->ram[at] = (uint8_t)(value & 0xffu);
    c->ram[at + 1] = (uint8_t)((value >> 8) & 0xffu);
}

/* Same boot-module fingerprint as ff8_state.c: avoids touching RAM while the
 * soft-reset disc-check module is still resident over stale save data. */
static int boot_module(const cpu_t *c) {
    uint64_t hash = UINT64_C(14695981039346656037);
    unsigned at;
    for (at = 0x98180u; at < 0x98280u; ++at)
        hash = (hash ^ c->ram[at]) * UINT64_C(1099511628211);
    return hash == UINT64_C(0x8a8acc91225252a6);
}

static int game_live(const cpu_t *c, unsigned *mode_out) {
    unsigned s = FF8_SAVEMAP;
    unsigned mode = read16(c, FF8_MODE);
    unsigned disc = c->ram[s + FF8_DISC];
    int live_mode = mode == 1 || mode == 2 || mode == 3 || mode == 6;
    if (mode_out) *mode_out = mode;
    if (!live_mode || read16(c, s + 2) != 0x08ffu || disc < 1 || disc > 4) return 0;
    return !boot_module(c);
}

void host_boosters_init(host_boosters_t *b, int speed_boost, int no_encounters, int battle_assist) {
    memset(b, 0, sizeof(*b));
    b->speed_boost = !!speed_boost;
    b->no_encounters = !!no_encounters;
    b->battle_assist = !!battle_assist;
    if (b->no_encounters || b->battle_assist) b->tainted = 1;
}

int host_boosters_gameplay_active(const host_boosters_t *b) {
    return b && (b->no_encounters || b->battle_assist);
}

int host_boosters_tainted(const host_boosters_t *b) {
    return b && (b->tainted || b->no_encounters || b->battle_assist);
}

void host_boosters_set_speed(host_boosters_t *b, int enabled) {
    if (b) b->speed_boost = !!enabled;
}

void host_boosters_set_gameplay(host_boosters_t *b, int no_encounters, int battle_assist) {
    if (!b) return;
    b->no_encounters = !!no_encounters;
    b->battle_assist = !!battle_assist;
    if (b->no_encounters || b->battle_assist) b->tainted = 1;
}

void host_boosters_mark_tainted(host_boosters_t *b) {
    if (b) b->tainted = 1;
}

double host_boosters_speed_scale(const host_boosters_t *b) {
    return (b && b->speed_boost) ? 3.0 : 1.0;
}

void host_boosters_frame(host_boosters_t *b, cpu_t *c) {
    unsigned mode = 0;
    if (!b || !c) return;
    if (!b->no_encounters && !b->battle_assist && !b->encounter_applied) return;
    if (!game_live(c, &mode)) {
        b->encounter_applied = 0;
        return;
    }

    if (b->no_encounters) {
        if (mode == 1 || mode == 2) {
            uint16_t cur = read16(c, FF8_ENCOUNTER_CONTROL);
            if (!b->encounter_applied) {
                b->encounter_saved = cur;
                b->encounter_applied = 1;
            }
            if (cur != FF8_ENCOUNTER_NONE) write16(c, FF8_ENCOUNTER_CONTROL, FF8_ENCOUNTER_NONE);
        }
    } else if (b->encounter_applied) {
        if ((mode == 1 || mode == 2) && read16(c, FF8_ENCOUNTER_CONTROL) == FF8_ENCOUNTER_NONE) {
            write16(c, FF8_ENCOUNTER_CONTROL,
                    b->encounter_saved == FF8_ENCOUNTER_NONE ? 0u : b->encounter_saved);
        }
        b->encounter_applied = 0;
    }

    if (b->battle_assist && mode == 3) {
        unsigned i;
        for (i = 0; i < 3; ++i) {
            unsigned member = c->ram[FF8_SAVEMAP + FF8_PARTY + i];
            unsigned off = i * FF8_BATTLE_SLOT_STRIDE;
            uint16_t max_hp, cur_hp, cur_atb;
            if (member == 255u) continue;
            max_hp = read16(c, FF8_BATTLE_MAX_HP + off);
            cur_hp = read16(c, FF8_BATTLE_CUR_HP + off);
            if (max_hp >= 1u && max_hp <= 9999u && cur_hp < max_hp) {
                write16(c, FF8_BATTLE_CUR_HP + off, max_hp);
            }
            if (max_hp >= 1u && max_hp <= 9999u) {
                cur_atb = read16(c, FF8_BATTLE_ATB + off);
                if (cur_atb < FF8_BATTLE_ATB_MAX) write16(c, FF8_BATTLE_ATB + off, FF8_BATTLE_ATB_MAX);
                c->ram[FF8_BATTLE_STATUS_AURA + off] |= FF8_BATTLE_AURA_BIT;
                {
                    unsigned ent = FF8_BATTLE_ENTITY_AURA + i * FF8_BATTLE_ENTITY_STRIDE;
                    uint16_t flags = read16(c, ent);
                    if (flags == 0u || (flags & FF8_BATTLE_AURA_BIT))
                        write16(c, ent, (uint16_t)(flags | FF8_BATTLE_AURA_BIT));
                }
            }
        }
    }
}
