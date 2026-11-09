#include "lionheart_generator.h"

#include <string.h>

#include "base.h"
#include "catalog.h"
#include "emit.h"
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
    if (options->timing != LHG_TIMING_EXACT)
        lhg_fail(&ctx, "only exact timing is implemented by the native generator");

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
    lhg_discover(&arena, program.main, NULL, 0, 1, &functions);

    lhg_emit_options emit_options = {options->mode, options->host_mode, options->timing};
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
