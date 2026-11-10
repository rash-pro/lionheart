/* The developer switch for the frame identity service: start it with the GTE's
 * recording entries bound, write its stream, print what it found. */
#ifndef HOST_FRAME_IDENTITY_HOST_H
#define HOST_FRAME_IDENTITY_HOST_H
#include "cpu.h"

/* dump may be NULL (summary only). linked: the host runs recompiled code
 * linked into it, which only a build with FF8_FRAME_IDENTITY_LINKED can
 * observe. */
int host_frame_identity_start(cpu_t *c, const char *dump, int linked);
/* A state was restored: observe its GPU and forget the history. */
void host_frame_identity_restored(cpu_t *c);
/* Unbind, flush and print the summary on stdout. Returns 0 if writing failed. */
int host_frame_identity_stop(void);
#endif
