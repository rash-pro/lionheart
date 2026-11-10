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
/* Linked: one tree compiled into the executable. Modules: one tree per set, loaded at run time. */
typedef enum { LHG_SHAPE_LINKED = 0, LHG_SHAPE_MODULES = 1 } lhg_shape;

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

/* One catalog file held by the host, named as in catalog_dir. Borrowed for the call. */
typedef struct {
    const char *name;
    const unsigned char *data;
    size_t size;
} lhg_catalog_file;

typedef struct {
    /* Directory holding overlays.json, overlays-all.json and files-disc*.json. */
    const char *catalog_dir;
    /* When nonzero, these replace catalog_dir; no catalog file is read from disk. */
    const lhg_catalog_file *catalog_files;
    size_t catalog_file_count;
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
    lhg_shape shape;
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

/* ---- One module at a time, for a host that builds them itself ----
 *
 * The full catalog (no legacy_overlays, no exe_path) in the module shape. A
 * host lists the sets its discs hold from catalog metadata alone, then opens a
 * session that reads the discs once and emits one set's module tree per call. */

/* The executable's module uses this index, as the module ABI does. */
#define LHG_MAIN_SET (-1)

typedef struct {
    /* Unique among the sets of one disc combination, but not across combinations. */
    int index;
    /* Bit n is set when disc n holds this image. The executable is on every disc. */
    unsigned discs;
    int effect;
    /* Load range; base is 0 when only the image bytes can establish it. */
    unsigned long base, size;
    /* Discovered instructions, a measure of compile cost; 0 until a session has read the discs. */
    unsigned long instructions;
    /* The catalog group, for display. */
    char name[32];
    /* SHA-256 of the catalog facts this set's code is generated from. Two sets
     * with the same identity always produce the same module. */
    char identity[65];
} lhg_set_info;

/* Reads no disc. disc_mask selects discs by number (bit n for disc n). The
 * array is malloc'd, executable first, then overlay sets by index. */
lhg_status lhg_plan_sets(const lhg_options *options, unsigned disc_mask, lhg_set_info **sets, size_t *count,
                         char *error, size_t error_size);

typedef struct lhg_session lhg_session;
/* Reads and verifies options->disc_paths and discovers every set. Writes
 * nothing: output_dir and assets_dir are ignored. */
lhg_status lhg_session_open(const lhg_options *options, lhg_session **session, char *error, size_t error_size);
/* The same rows lhg_plan_sets returns for these discs, owned by the session. */
size_t lhg_session_sets(const lhg_session *session, const lhg_set_info **sets);
/* Emits set number `set` (a row of lhg_session_sets) as one module tree:
 * unit_NNN.c files and tables.c, into an existing directory, or nowhere when
 * directory is NULL. key receives a SHA-256 of everything the module's code
 * depends on and nothing else: identical code from two sets, or two discs,
 * gets one key although the descriptor names the set it was emitted for.
 * Calls on one session must not overlap. */
lhg_status lhg_session_emit(lhg_session *session, size_t set, const char *directory, char key[65], char *error,
                            size_t error_size);
/* Whether the image of set `set` holds `size` bytes equal to `bytes` at guest address pc. */
int lhg_session_contains(const lhg_session *session, size_t set, unsigned long pc, const unsigned char *bytes, size_t size);
void lhg_session_close(lhg_session *session);

#ifdef __cplusplus
}
#endif

#endif
