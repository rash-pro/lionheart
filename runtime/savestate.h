#ifndef FF8_SAVESTATE_H
#define FF8_SAVESTATE_H
#include "cpu.h"
#include <stdio.h>
#define SAVESTATE_MAX (16u * 1024u * 1024u)
typedef struct {
    char build[64];
    uint32_t exe, disc, modes;
    uint8_t disc_hash[20];
    uint64_t requested_vblank, requested_instructions;
} savestate_identity_t;
typedef int (*savestate_disc_open_t)(cpu_t *, unsigned, const uint8_t *, void *, char *, size_t);
int savestate_write(cpu_t *, const char *, const savestate_identity_t *, char *, size_t);
/* Returns a separately allocated session; on failure the old session is untouched. */
cpu_t *savestate_read(const cpu_t *, const char *, savestate_identity_t *,
                      savestate_disc_open_t, void *, char *, size_t);
void savestate_dispose(cpu_t *);
void savestate_handler(int (*)(cpu_t *, void *), void *);
/* One host session owns the request; the idle dispatcher only compares this pointer. */
extern cpu_t *savestate_pending;
void savestate_request(cpu_t *);
void savestate_cancel(cpu_t *);
int savestate_ready(cpu_t *);
int savestate_boundary(cpu_t *);
#endif
