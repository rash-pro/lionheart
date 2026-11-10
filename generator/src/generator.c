#include "lionheart_generator.h"

#include <string.h>

#include "base.h"
#include "cache.h"
#include "catalog.h"
#include "emit.h"
#include "json.h"
#include "locals.h"
#include "platform.h"

lhg_status lhg_generate(const lhg_options *options, lhg_summary *summary, char *error, size_t error_size)
{
    lhg_ctx ctx;
    lhg_ctx_init(&ctx);
    if (summary)
        memset(summary, 0, sizeof *summary);
    ctx.progress = options->progress;
    ctx.progress_user = options->progress_user;
    ctx.catalog_files = options->catalog_files;
    ctx.catalog_file_count = options->catalog_file_count;
    /* Declared before setjmp and assigned once; never read after the longjmp path. */
    lhg_arena arena;
    if (setjmp(ctx.bail)) {
        if (error && error_size)
            snprintf(error, error_size, "%s", ctx.error);
        lhg_ctx_release(&ctx);
        return ctx.status;
    }
    lhg_arena_init(&arena, &ctx);

    if (!options->output_dir || (!options->catalog_dir && !options->catalog_file_count))
        lhg_fail(&ctx, "output directory and catalog directory are required");
    if ((options->exe_path != NULL) == (options->disc_count > 0))
        lhg_fail(&ctx, "give either an EXE or discs, not both or neither");
    if (options->disc_count < 0 || options->disc_count > 4)
        lhg_fail(&ctx, "--disc may be supplied at most four times");
    if (options->use_cache && (options->exe_path || !options->legacy_overlays))
        lhg_fail(&ctx, "--cache requires the legacy disc catalog");
    if (options->mode != LHG_MODE_FAST && options->mode != LHG_MODE_TRACE)
        lhg_fail(&ctx, "unknown emission mode: %d", options->mode);
    if (options->host_mode != LHG_HOST_PRECISE && options->host_mode != LHG_HOST_FAST)
        lhg_fail(&ctx, "unknown host mode: %d", options->host_mode);
    if (options->timing < LHG_TIMING_EXACT || options->timing > LHG_TIMING_LOOSE)
        lhg_fail(&ctx, "unknown timing policy: %d", options->timing);
    if (options->timing != LHG_TIMING_EXACT && options->mode == LHG_MODE_TRACE)
        lhg_fail(&ctx, "experimental timing requires fast emission");
    if (options->shape != LHG_SHAPE_LINKED && options->shape != LHG_SHAPE_MODULES)
        lhg_fail(&ctx, "unknown output shape: %d", options->shape);
    if (options->shape == LHG_SHAPE_MODULES && (options->mode != LHG_MODE_FAST || options->host_mode != LHG_HOST_PRECISE || options->timing != LHG_TIMING_EXACT))
        lhg_fail(&ctx, "module output requires fast emission, a precise host and exact timing");
    /* The developer cache describes a linked tree; modules get their own cache. */
    if (options->use_cache && options->shape != LHG_SHAPE_LINKED)
        lhg_fail(&ctx, "--cache requires the linked output shape");

    const lhg_register_cache *selection = options->register_cache;
    if (selection)
        lhg_cache_validate(&ctx, selection);
    else if (options->timing != LHG_TIMING_EXACT)
        selection = lhg_cache_read(&arena, lhg_join_path(&arena, options->catalog_dir, "play-register-cache.json"));

    const char *assets = options->assets_dir;
    if (!assets)
        assets = lhg_join_path(&arena, options->output_dir, "extracted");

    lhg_program program;
    memset(&program, 0, sizeof program);
    if (options->exe_path) {
        lhg_load_exe(&arena, options->catalog_dir, options->exe_path, &program);
    } else if (options->legacy_overlays) {
        lhg_load_legacy_discs(&arena, options->catalog_dir, options->disc_paths, options->disc_count, assets, &program, options->use_cache);
    } else {
        lhg_compile_catalog(&arena, options->catalog_dir, options->disc_paths, options->disc_count, assets, &program);
    }

    char cache_key[65];
    if (options->use_cache) {
        if (!(program.disc_mask & (1u << 1)))
            lhg_fail(&ctx, "disc 1 is required");
        lhg_cache_key(&arena, options, &program, cache_key);
        if (lhg_output_cache_hit(&arena, options->output_dir, cache_key)) {
            if (summary) {
                summary->cache_hit = 1;
                summary->overlay_sets = (unsigned long)program.overlay_count;
                size_t size;
                uint8_t *data = lhg_read_file(&ctx, lhg_join_path(&arena, options->output_dir, "manifest.json"), &size);
                const lhg_json *manifest = lhg_json_parse(&arena, (const char *)data, size, "manifest.json");
                summary->functions = (unsigned long)lhg_json_int(&ctx, lhg_json_find(manifest, "functions"), "functions");
                summary->instructions = (unsigned long)lhg_json_int(&ctx, lhg_json_find(manifest, "instructions"), "instructions");
                const lhg_json *tables = lhg_json_require(&ctx, manifest, "jump_tables", "manifest.json");
                if (tables->type != LHG_JSON_OBJECT)
                    lhg_fail(&ctx, "manifest.json: jump_tables must be an object");
                summary->jump_tables = (unsigned long)tables->as.object.count;
            }
            lhg_arena_release(&arena);
            lhg_ctx_release(&ctx);
            if (error && error_size) error[0] = 0;
            return LHG_OK;
        }
    }
    /* Even an uncached writer must retire an earlier receipt before it changes
     * this directory. A cancelled or killed emission can never publish a hit. */
    lhg_output_cache_invalidate(&arena, options->output_dir);
    lhg_progress(&ctx, "generation", 0, 1);
    if (!options->exe_path && options->legacy_overlays)
        lhg_discover_legacy(&arena, &program);

    lhg_functions functions;
    int experimental = options->timing != LHG_TIMING_EXACT;
    lhg_discover(&arena, program.main, experimental ? program.main->timing_entry_points : NULL,
                 experimental ? program.main->timing_entry_point_count : 0, 1, &functions);

    lhg_emit_options emit_options = {options->mode, options->host_mode, options->timing, selection, options->shape};
    lhg_emit_result result;
    memset(&result, 0, sizeof result);
    if (options->shape == LHG_SHAPE_MODULES)
        lhg_emit_modules(&arena, program.main, &functions, options->output_dir, program.overlays, program.overlay_count, &result);
    else
        lhg_emit(&arena, program.main, &functions, options->output_dir, program.overlays, program.overlay_count, &emit_options, &result);
    if (options->use_cache)
        lhg_output_cache_save(&arena, options->output_dir, cache_key);

    if (summary) {
        summary->functions = (unsigned long)result.functions;
        summary->instructions = (unsigned long)result.instructions;
        summary->jump_tables = (unsigned long)result.jump_tables;
        summary->overlay_sets = (unsigned long)program.overlay_count;
        summary->full_catalog = !options->exe_path && !options->legacy_overlays;
    }
    lhg_arena_release(&arena);
    lhg_ctx_release(&ctx);
    if (error && error_size)
        error[0] = 0;
    return LHG_OK;
}
