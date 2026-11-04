#include "state_bridge.h"
#include "achievements.h"
#include "ff8_state.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
static char last_event[8192];
static unsigned events;
static void event(void *context, const char *json) {
    (void)context;
    snprintf(last_event, sizeof(last_event), "%s", json);
    ++events;
}
cpu_t *test_state_cpu(void) { return calloc(1, sizeof(cpu_t)); }
void test_state_free(cpu_t *c) { free(c); }
void test_state_write(cpu_t *c, unsigned address, unsigned value, unsigned size) {
    unsigned i;
    for (i = 0; i < size; ++i) {
        if (address + i < RAM_SIZE) c->ram[address + i] = value >> (i * 8);
        else if (address + i < RAM_SIZE + sizeof(c->scratch)) c->scratch[address + i - RAM_SIZE] = value >> (i * 8);
    }
}
host_achievements_t *test_achievements_open(const char *patch, const char *progress, const char *disc) {
    events = 0; last_event[0] = 0;
    return host_achievements_open(patch, NULL, progress, disc, event, NULL);
}
const char *test_last_event(void) { return last_event; }
unsigned test_events(void) { return events; }
