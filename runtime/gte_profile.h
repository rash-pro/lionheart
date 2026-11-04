#ifndef FF8_GTE_PROFILE_H
#define FF8_GTE_PROFILE_H

/* Diagnostic counters only. Never add per-command work to timing builds. */
#define gte_profile_command(c, opcode) ((void)0)
#define GTE_PROFILE_READ(c) ((void)0)
#define GTE_PROFILE_WRITE(c) ((void)0)
#endif
