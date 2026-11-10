#ifndef LHG_CACHE_H
#define LHG_CACHE_H
#include "catalog.h"

void lhg_cache_key(lhg_arena *arena, const lhg_options *options, const lhg_program *program, char key[65]);
/* Compares a complete receipt, including the current set of output names. */
int lhg_output_cache_hit(lhg_arena *arena, const char *output, const char *key);
void lhg_output_cache_invalidate(lhg_arena *arena, const char *output);
void lhg_output_cache_save(lhg_arena *arena, const char *output, const char *key);
#endif
