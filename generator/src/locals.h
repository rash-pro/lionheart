/* Register selection is data; lowering runs before overlay symbol renaming. */
#ifndef LHG_LOCALS_H
#define LHG_LOCALS_H

#include "emit.h"

int lhg_cache_contains(const char *const *names, size_t count, const char *name);
void lhg_cache_validate(lhg_ctx *ctx, const lhg_register_cache *selection);
const lhg_register_cache *lhg_cache_parse(lhg_arena *arena, const lhg_json *json);
const lhg_register_cache *lhg_cache_read(lhg_arena *arena, const char *path);
int lhg_cache_qualify(const lhg_function *function);
void lhg_cache_lower(lhg_arena *arena, const lhg_text *source, int cap, lhg_text *out);

#endif
