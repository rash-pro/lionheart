/* The developer switch for the frame identity service: start it with the GTE's
 * recording entries bound, write its stream, print what it found. */
#ifndef HOST_FRAME_IDENTITY_HOST_H
#define HOST_FRAME_IDENTITY_HOST_H
#include "cpu.h"

/* dump may be NULL (summary only). oracle: a report path, for developer
 * builds with FF8_FRAME_ORACLE, or NULL. linked: the host runs recompiled code
 * linked into it, which only a build with FF8_FRAME_IDENTITY_LINKED can
 * observe. record: a path for a recording of VBlanks from..until (0: to the
 * end) that the frames can be drawn again from (runtime/frame_record.h), or NULL. */
int host_frame_identity_start(cpu_t *c, const char *dump, const char *oracle, int linked,
                              const char *record, uint64_t from, uint64_t until);
/* A state was restored: observe its GPU and forget the history. */
void host_frame_identity_restored(cpu_t *c);
/* Unbind, flush and print the summary on stdout. Returns 0 if writing failed. */
int host_frame_identity_stop(void);
#endif
