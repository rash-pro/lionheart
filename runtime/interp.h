#ifndef FF8_INTERP_H
#define FF8_INTERP_H
#include "cpu.h"

/* The interpreter's GTE command entry: gte_cmd, unless a host observing
 * projections has pointed it at a recording entry for as long as it observes.
 * Recompiled code reaches the GTE through lh_api or direct calls instead. */
extern void (*interp_gte_command)(cpu_t *c, uint32_t opcode);
#endif
