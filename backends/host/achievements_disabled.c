#include "achievements.h"
#include <stdio.h>
host_achievements_t *host_achievements_open(const char *patch, const char *card, const char *progress,
    const char *disc, host_achievement_event_t event, void *context) {
    (void)patch; (void)card; (void)progress; (void)disc; (void)event; (void)context;
    fprintf(stderr,"Achievements disabled in this build\n"); return NULL;
}
void host_achievements_frame(host_achievements_t *a, const cpu_t *c) { (void)a; (void)c; }
int host_achievements_close(host_achievements_t *a) { (void)a; return 1; }
uint32_t host_achievements_read(uint32_t a, uint8_t *b, uint32_t n, void *c) {
    (void)a; (void)b; (void)n; (void)c; return 0;
}
