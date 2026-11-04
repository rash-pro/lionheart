#include "cpu.h"
#include <stdlib.h>
#include <string.h>

cpu_t *gte_test_new(void) { return calloc(1, sizeof(cpu_t)); }
void gte_test_free(cpu_t *c) { free(c); }
void gte_test_reset(cpu_t *c) {
    memset(c->gte_data, 0, sizeof(c->gte_data));
    memset(c->gte_ctrl, 0, sizeof(c->gte_ctrl));
    c->gte_commands = 0;
}
uint64_t gte_test_count(const cpu_t *c) { return c->gte_commands; }
