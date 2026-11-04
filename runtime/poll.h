#ifndef FF8_POLL_H
#define FF8_POLL_H
#include "cpu.h"

/* Immutable source words avoid a pointer into the getter's native page. */
typedef struct {
    uint32_t head, getter, address, result;
    uint32_t words[8];
} poll_site_t;
void c_poll_skip(cpu_t *, const poll_site_t *);
#endif
