/* C emission: turns discovered functions into the generated translation units. */
#ifndef LHG_EMIT_H
#define LHG_EMIT_H

#include "catalog.h"
#include "json.h"

/* Instruction effects shared by every emission mode. */
typedef struct {
    lhg_lines lines;
    int dst, load;
    const char *value, *loaded;
} lhg_effect;

void lhg_effects(lhg_arena *arena, const lhg_insn *insn, lhg_effect *effect);
const char *lhg_gte_read_expression(lhg_arena *arena, unsigned reg, int control);
void lhg_gte_write_statement(lhg_lines *lines, unsigned reg, const char *value, int control);
/* The trace-mode retirement for a single non-control instruction. */
void lhg_operation(lhg_arena *arena, lhg_lines *out, const lhg_insn *insn);

/* Everything an emitter needs about one function, computed once for the
 * several variants a generated function can be emitted in. */
typedef struct {
    const lhg_function *function;
    int32_t *distance;
    int32_t event_gap;
    uint8_t *is_leader;
    int32_t *windows;
    /* Python set-iteration order never reaches the output, so plain flags per
     * instruction replace the original sets. */
    uint8_t *full_entries, *compact_entries;
} lhg_analysis;

void lhg_analyze(lhg_arena *arena, const lhg_function *function, lhg_analysis *analysis);

/* The functions a call may target directly: this set's, then the main executable's. */
typedef struct {
    const lhg_functions *own;
    const lhg_functions *main;
} lhg_available;

const lhg_function *lhg_available_find(const lhg_available *available, uint32_t pc);

/* Appends one emitted variant to out; no trailing newline handling beyond the emitter's own. */
void lhg_emit_fast_variant(lhg_arena *arena, const lhg_analysis *analysis, const lhg_available *available, int trace,
                           int compact, int poll_forward, int direct_memory, lhg_text *out);
void lhg_emit_precise(lhg_arena *arena, const lhg_analysis *analysis, lhg_text *out);
void lhg_emit_timing(lhg_arena *arena, const lhg_analysis *analysis, const lhg_available *available,
                     lhg_timing timing, int inline_blocks, int fuse_forward, lhg_text *out);

typedef struct {
    lhg_mode mode;
    lhg_host_mode host_mode;
    lhg_timing timing;
    const lhg_register_cache *register_cache;
} lhg_emit_options;

typedef struct {
    size_t functions, instructions, jump_tables;
} lhg_emit_result;

void lhg_emit(lhg_arena *arena, const lhg_image *image, const lhg_functions *functions, const char *output,
              const lhg_overlay_set *overlays, size_t overlay_count, const lhg_emit_options *options,
              lhg_emit_result *result);

#endif
