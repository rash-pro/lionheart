#include "lionheart_generator.h"

#include <string.h>

#include "base.h"
#include "catalog.h"
#include "emit.h"
#include "locals.h"
#include "platform.h"

lhg_status lhg_generate(const lhg_options *options, lhg_summary *summary, char *error, size_t error_size)
{
    lhg_ctx ctx;
    lhg_ctx_init(&ctx);
    ctx.progress = options->progress;
    ctx.progress_user = options->progress_user;
    /* Declared before setjmp and assigned once; never read after the longjmp path. */
    lhg_arena arena;
    if (setjmp(ctx.bail)) {
        if (error && error_size)
            snprintf(error, error_size, "%s", ctx.error);
        lhg_ctx_release(&ctx);
        return ctx.status;
    }
    lhg_arena_init(&arena, &ctx);

    if (!options->output_dir || !options->catalog_dir)
        lhg_fail(&ctx, "output directory and catalog directory are required");
    if ((options->exe_path != NULL) == (options->disc_count > 0))
        lhg_fail(&ctx, "give either an EXE or discs, not both or neither");
    if (options->disc_count > 4)
        lhg_fail(&ctx, "--disc may be supplied at most four times");
    if (options->mode != LHG_MODE_FAST && options->mode != LHG_MODE_TRACE)
        lhg_fail(&ctx, "unknown emission mode: %d", options->mode);
    if (options->host_mode != LHG_HOST_PRECISE && options->host_mode != LHG_HOST_FAST)
        lhg_fail(&ctx, "unknown host mode: %d", options->host_mode);
    if (options->timing < LHG_TIMING_EXACT || options->timing > LHG_TIMING_LOOSE)
        lhg_fail(&ctx, "unknown timing policy: %d", options->timing);
    if (options->timing != LHG_TIMING_EXACT && options->mode == LHG_MODE_TRACE)
        lhg_fail(&ctx, "experimental timing requires fast emission");

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
        lhg_load_legacy_discs(&arena, options->catalog_dir, options->disc_paths, options->disc_count, assets, &program);
    } else {
        lhg_compile_catalog(&arena, options->catalog_dir, options->disc_paths, options->disc_count, assets, &program);
    }

    lhg_functions functions;
    int experimental = options->timing != LHG_TIMING_EXACT;
    lhg_discover(&arena, program.main, experimental ? program.main->timing_entry_points : NULL,
                 experimental ? program.main->timing_entry_point_count : 0, 1, &functions);

    lhg_emit_options emit_options = {options->mode, options->host_mode, options->timing, selection};
    lhg_emit_result result;
    memset(&result, 0, sizeof result);
    lhg_emit(&arena, program.main, &functions, options->output_dir, program.overlays, program.overlay_count, &emit_options, &result);

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
