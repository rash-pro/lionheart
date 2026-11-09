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

typedef struct {
    const lhg_image *main;
    lhg_overlay_set *overlays;
    size_t overlay_count;
} lhg_program;

/* A verified EXE from the filesystem (overlays are the empty set). */
void lhg_load_exe(lhg_arena *arena, const char *catalog_dir, const char *path, lhg_program *program);
void lhg_load_legacy_discs(lhg_arena *arena, const char *catalog_dir, const char *const *cues, int cue_count,
                           const char *assets_dir, lhg_program *program);
/* The load address of a battle-effect image with none configured, and what proves it. Fails on a tie. */
uint32_t lhg_infer_effect_base(lhg_arena *arena, const uint8_t *data, size_t size, const char *group_name,
                               lhg_json *evidence);
void lhg_compile_catalog(lhg_arena *arena, const char *catalog_dir, const char *const *cues, int cue_count,
                         const char *assets_dir, lhg_program *program);

#endif
