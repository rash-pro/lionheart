/* Loads verified inputs into images and discovered functions, kept apart from
 * emission so another backend can use the same results. */
#ifndef LHG_CATALOG_H
#define LHG_CATALOG_H

#include "base.h"
#include "discover.h"
#include "json.h"

typedef struct {
    int index;
    const lhg_image *image;
    lhg_functions functions;
} lhg_overlay_set;

/* One set the full catalog gives for the present discs, known before any disc is read. */
typedef struct {
    size_t group;
    const lhg_json *group_json, *location;
    const char *group_name, *loaded_sha1;
    int index, disc, top, has_top;
    /* No inventory entry: the loader fails here once the bytes are verified. */
    int missing;
    /* Discs holding these loaded bytes. */
    unsigned discs;
} lhg_plan_row;

typedef struct {
    const lhg_image *main;
    lhg_overlay_set *overlays;
    size_t overlay_count;
    unsigned disc_mask;
    /* The full catalog's plan, one row per overlay set. */
    const lhg_plan_row *plan;
    size_t plan_count;
} lhg_program;

/* A verified EXE from the filesystem (overlays are the empty set). */
void lhg_load_exe(lhg_arena *arena, const char *catalog_dir, const char *path, lhg_program *program);
void lhg_load_legacy_discs(lhg_arena *arena, const char *catalog_dir, const char *const *cues, int cue_count,
                           const char *assets_dir, lhg_program *program, int verify_exe_identity);
/* Discovery is deferred until after the verified-input cache check. */
void lhg_discover_legacy(lhg_arena *arena, lhg_program *program);
/* The load address of a battle-effect image with none configured, and what proves it. Fails on a tie. */
uint32_t lhg_infer_effect_base(lhg_arena *arena, const uint8_t *data, size_t size, const char *group_name,
                               lhg_json *evidence);
/* The full catalog's sets for the discs in disc_mask (bit n for disc n), from metadata alone. */
void lhg_plan_catalog(lhg_arena *arena, const char *catalog_dir, unsigned disc_mask, lhg_plan_row **rows, size_t *count);
/* What a set's code is generated from, as named by the catalog; see lhg_set_info. */
void lhg_plan_identity(lhg_ctx *ctx, const lhg_plan_row *row, char hex[65]);
/* The configured load address, or 0 when the image bytes decide it. */
uint32_t lhg_plan_base(lhg_ctx *ctx, const lhg_plan_row *row);
/* assets_dir NULL keeps every extracted image in memory. */
void lhg_compile_catalog(lhg_arena *arena, const char *catalog_dir, const char *const *cues, int cue_count,
                         const char *assets_dir, lhg_program *program);

#endif
