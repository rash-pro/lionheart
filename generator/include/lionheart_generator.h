/* Recompiler generator: turns the player's own discs into C99 that the
 * runtime links. Every call owns its state, so a host may run it on a worker
 * thread; failures and cancellation always return through lhg_generate. */
#ifndef LIONHEART_GENERATOR_H
#define LIONHEART_GENERATOR_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum { LHG_OK = 0, LHG_FAILED = 1, LHG_CANCELLED = 2 } lhg_status;
typedef enum { LHG_MODE_FAST = 0, LHG_MODE_TRACE = 1 } lhg_mode;
typedef enum { LHG_HOST_PRECISE = 0, LHG_HOST_FAST = 1 } lhg_host_mode;
typedef enum { LHG_TIMING_EXACT = 0, LHG_TIMING_BLOCK = 1, LHG_TIMING_LOOSE = 2 } lhg_timing;

/* Returns nonzero to cancel. Called between units of work on the caller's thread. */
typedef int (*lhg_progress_fn)(void *user, const char *stage, unsigned long done, unsigned long total);

/* Selected generated symbols, including their owning overlay prefix. The
 * selection is borrowed for the duration of lhg_generate. */
typedef struct {
    /* 1..8. Functions must be nonempty and unique; fused names are a subset. */
    int max_registers;
    const char *const *functions;
    size_t function_count;
    const char *const *fuse_forward;
    size_t fuse_forward_count;
} lhg_register_cache;

typedef struct {
    /* Directory holding overlays.json, overlays-all.json and files-disc*.json. */
    const char *catalog_dir;
    /* Exactly one of exe_path or disc_paths[0..disc_count) is used. */
    const char *exe_path;
    const char *disc_paths[4];
    int disc_count;
    const char *output_dir;
    /* NULL selects OUTPUT/extracted. */
    const char *assets_dir;
    int legacy_overlays;
    lhg_mode mode;
    lhg_host_mode host_mode;
    lhg_timing timing;
    lhg_progress_fn progress;
    void *progress_user;
    /* NULL uses the catalog's play-register-cache.json for experimental timing.
     * Exact timing accepts a selection but does not apply the lowering. */
    const lhg_register_cache *register_cache;
    /* Cache the developer (legacy disc) catalog. Inputs are verified on every
     * call, including hits. Requires disc 1. Callers must own output_dir
     * exclusively until the call returns. Old setup caches are never reused. */
    int use_cache;
} lhg_options;

typedef struct {
    unsigned long functions;
    unsigned long instructions;
    unsigned long jump_tables;
    unsigned long overlay_sets;
    int full_catalog;
    int cache_hit;
} lhg_summary;

/* Generates into options->output_dir. error may be NULL. summary may be NULL. */
lhg_status lhg_generate(const lhg_options *options, lhg_summary *summary, char *error, size_t error_size);
/* Source identity compiled by CMake, shared with the restoration ABI. */
const char *lhg_generator_fingerprint(void);

#ifdef __cplusplus
}
#endif

#endif
