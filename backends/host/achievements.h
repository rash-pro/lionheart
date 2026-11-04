#ifndef FF8_HOST_ACHIEVEMENTS_H
#define FF8_HOST_ACHIEVEMENTS_H
#include "cpu.h"
typedef struct host_achievements host_achievements_t;
typedef void (*host_achievement_event_t)(void *, const char *);
host_achievements_t *host_achievements_open(const char *patch, const char *card,
    const char *progress, const char *disc, host_achievement_event_t event, void *context);
void host_achievements_frame(host_achievements_t *, const cpu_t *);
int host_achievements_close(host_achievements_t *);
/* RA's linear PlayStation address space; never invokes MMIO accessors. */
uint32_t host_achievements_read(uint32_t address, uint8_t *buffer, uint32_t count, void *cpu);
#endif
