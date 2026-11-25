#ifndef FF8_HOST_BOOSTERS_H
#define FF8_HOST_BOOSTERS_H
#include "cpu.h"
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

/* Remaster-style opt-in gameplay boosters and 3x speed toggle.
 * All features default to off. When all gameplay boosters are off, guest RAM
 * is never written. Enabling any gameplay booster permanently taints the
 * session for RetroAchievements lockout. */
typedef struct {
    int speed_boost;      /* 3x capped speed toggle (pacing only; does not taint RA) */
    int no_encounters;    /* Enc-None equivalent on field and world map */
    int battle_assist;    /* Full HP, full ATB, and Limit Break ready in battle */
    int tainted;          /* Irreversible once any gameplay booster is enabled or loaded */
    int encounter_applied;
    uint16_t encounter_saved;
} host_boosters_t;

void host_boosters_init(host_boosters_t *, int speed_boost, int no_encounters, int battle_assist);
int host_boosters_gameplay_active(const host_boosters_t *);
int host_boosters_tainted(const host_boosters_t *);
void host_boosters_set_speed(host_boosters_t *, int enabled);
void host_boosters_set_gameplay(host_boosters_t *, int no_encounters, int battle_assist);
void host_boosters_mark_tainted(host_boosters_t *);
double host_boosters_speed_scale(const host_boosters_t *);
/* Call once per guest VBlank. Safe no-op when no gameplay booster is active. */
void host_boosters_frame(host_boosters_t *, cpu_t *);

#ifdef __cplusplus
}
#endif
#endif
