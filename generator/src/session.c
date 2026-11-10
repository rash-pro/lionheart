/* One module at a time: the catalog plan without reading discs, and a session
 * that reads them once and then emits any single set on request. A host that
 * builds modules itself keeps the program in memory instead of a whole tree on
 * disk, and names what it builds by the keys computed here. */
#include "lionheart_generator.h"

#include <stdlib.h>
#include <string.h>

#include "base.h"
#include "catalog.h"
#include "emit.h"
#include "sha256.h"

struct lhg_session {
    lhg_ctx ctx;
    lhg_arena arena;
    lhg_program program;
    lhg_functions main_functions;
    lhg_set_info *sets;
    /* Per row: its first overlay set and how many it has; a code prefix has one per variant. */
    const lhg_overlay_set **overlays;
    size_t *members;
    size_t count;
};

static void report(const lhg_ctx *ctx, char *error, size_t error_size)
{
    if (error && error_size)
        snprintf(error, error_size, "%s", ctx->status == LHG_OK ? "" : ctx->error);
}

static void check_options(lhg_ctx *ctx, const lhg_options *options)
{
    if (!options->catalog_dir && !options->catalog_file_count)
        lhg_fail(ctx, "a catalog directory or embedded catalog is required");
    if (options->exe_path || options->legacy_overlays || options->use_cache)
        lhg_fail(ctx, "single modules come from the full disc catalog");
    if (options->shape != LHG_SHAPE_MODULES || options->mode != LHG_MODE_FAST || options->host_mode != LHG_HOST_PRECISE ||
        options->timing != LHG_TIMING_EXACT)
        lhg_fail(ctx, "single modules require the module shape, fast emission, a precise host and exact timing");
}

static void main_info(lhg_set_info *info, unsigned discs)
{
    lhg_sha256 hash;
    memset(info, 0, sizeof *info);
    info->index = LHG_MAIN_SET;
    info->discs = discs;
    snprintf(info->name, sizeof info->name, "main");
    /* The executable is fixed by the cache that holds its module. */
    lhg_sha256_init(&hash);
    lhg_sha256_add(&hash, "lionheart-set-1\nmain\n", 21);
    lhg_sha256_finish(&hash, info->identity);
}

static void row_info(lhg_ctx *ctx, const lhg_plan_row *row, lhg_set_info *info)
{
    memset(info, 0, sizeof *info);
    info->index = row->index;
    info->discs = row->discs;
    info->effect = !strncmp(row->group_name, "effect", 6);
    info->base = lhg_plan_base(ctx, row);
    info->size = lhg_plan_size(ctx, row);
    snprintf(info->name, sizeof info->name, "%s", row->group_name);
    lhg_plan_identity(ctx, row, info->identity);
}

static unsigned long instructions(const lhg_functions *functions)
{
    unsigned long total = 0;
    for (size_t row = 0; row < functions->count; row++)
        total += functions->items[row].insn_count;
    return total;
}

static int compare_info(const void *a, const void *b)
{
    int x = ((const lhg_set_info *)a)->index, y = ((const lhg_set_info *)b)->index;
    return x < y ? -1 : x > y;
}

lhg_status lhg_plan_sets(const lhg_options *options, unsigned disc_mask, lhg_set_info **sets, size_t *count, char *error,
                         size_t error_size)
{
    lhg_ctx ctx;
    lhg_ctx_init(&ctx);
    *sets = NULL;
    *count = 0;
    ctx.catalog_files = options->catalog_files;
    ctx.catalog_file_count = options->catalog_file_count;
    /* Declared before setjmp and assigned once; never read after the longjmp path. */
    lhg_arena arena;
    if (setjmp(ctx.bail)) {
        report(&ctx, error, error_size);
        lhg_ctx_release(&ctx);
        return ctx.status;
    }
    lhg_arena_init(&arena, &ctx);
    check_options(&ctx, options);
    if (!(disc_mask & 0x1Eu) || disc_mask & ~0x1Eu)
        lhg_fail(&ctx, "the disc mask must select discs 1 to 4");
    lhg_plan_row *rows;
    size_t row_count;
    lhg_plan_catalog(&arena, options->catalog_dir, disc_mask, &rows, &row_count);
    lhg_set_info *staged = lhg_alloc(&ctx, (row_count + 1) * sizeof *staged);
    main_info(&staged[0], disc_mask);
    for (size_t row = 0; row < row_count; row++)
        row_info(&ctx, &rows[row], &staged[row + 1]);
    qsort(staged + 1, row_count, sizeof *staged, compare_info);
    lhg_set_info *result = malloc((row_count + 1) * sizeof *result);
    if (!result)
        lhg_fail(&ctx, "out of memory");
    memcpy(result, staged, (row_count + 1) * sizeof *result);
    *sets = result;
    *count = row_count + 1;
    lhg_ctx_release(&ctx);
    report(&ctx, error, error_size);
    return LHG_OK;
}

lhg_status lhg_session_open(const lhg_options *options, lhg_session **result, char *error, size_t error_size)
{
    *result = NULL;
    lhg_session *session = calloc(1, sizeof *session);
    if (!session) {
        if (error && error_size)
            snprintf(error, error_size, "out of memory");
        return LHG_FAILED;
    }
    lhg_ctx *ctx = &session->ctx;
    lhg_ctx_init(ctx);
    ctx->progress = options->progress;
    ctx->progress_user = options->progress_user;
    ctx->catalog_files = options->catalog_files;
    ctx->catalog_file_count = options->catalog_file_count;
    if (setjmp(ctx->bail)) {
        lhg_status status = ctx->status;
        report(ctx, error, error_size);
        lhg_ctx_release(ctx);
        free(session);
        return status;
    }
    lhg_arena_init(&session->arena, ctx);
    check_options(ctx, options);
    if (options->disc_count < 1 || options->disc_count > 4)
        lhg_fail(ctx, "between one and four discs are required");
    lhg_compile_catalog(&session->arena, options->catalog_dir, options->disc_paths, options->disc_count, NULL, &session->program);
    lhg_discover(&session->arena, session->program.main, NULL, 0, 1, &session->main_functions);

    const lhg_program *program = &session->program;
    session->sets = lhg_alloc(ctx, (program->overlay_count + 1) * sizeof *session->sets);
    session->overlays = lhg_zalloc(ctx, program->overlay_count + 1, sizeof *session->overlays);
    session->members = lhg_zalloc(ctx, program->overlay_count + 1, sizeof *session->members);
    session->count = 1;
    unsigned discs = 0;
    for (size_t row = 0; row < program->plan_count; row++)
        discs |= program->plan[row].discs;
    main_info(&session->sets[0], discs);
    session->sets[0].instructions = instructions(&session->main_functions);
    for (size_t set = 0; set < program->overlay_count; set++) {
        const lhg_overlay_set *overlay = &program->overlays[set];
        if (overlay->shares_row) {
            session->members[session->count - 1]++;
            session->sets[session->count - 1].instructions += instructions(&overlay->functions);
            continue;
        }
        const lhg_plan_row *row = NULL;
        for (size_t at = 0; at < program->plan_count && !row; at++)
            if (program->plan[at].index == overlay->index)
                row = &program->plan[at];
        if (!row)
            lhg_fail(ctx, "set %d is missing from the catalog plan", overlay->index);
        lhg_set_info *info = &session->sets[session->count];
        row_info(ctx, row, info);
        /* Now that the bytes are known, an inferred load address is too. */
        info->base = overlay->image->base;
        info->instructions = instructions(&overlay->functions);
        session->overlays[session->count] = overlay;
        session->members[session->count] = 1;
        session->count++;
    }
    *result = session;
    report(ctx, error, error_size);
    return LHG_OK;
}

size_t lhg_session_sets(const lhg_session *session, const lhg_set_info **sets)
{
    *sets = session->sets;
    return session->count;
}

lhg_status lhg_session_emit(lhg_session *session, size_t set, const char *directory, char key[65], char *error,
                            size_t error_size)
{
    lhg_ctx ctx;
    lhg_ctx_init(&ctx);
    /* Declared before setjmp and assigned once; never read after the longjmp path. */
    lhg_arena arena;
    if (setjmp(ctx.bail)) {
        report(&ctx, error, error_size);
        lhg_ctx_release(&ctx);
        return ctx.status;
    }
    lhg_arena_init(&arena, &ctx);
    if (set >= session->count)
        lhg_fail(&ctx, "no set %lu in this session", (unsigned long)set);
    const lhg_image *exe = session->program.main;
    if (!set) {
        lhg_module_part part = {exe, &session->main_functions};
        lhg_emit_module_set(&arena, exe, LHG_MAIN_SET, &part, 1, directory, key);
    } else {
        size_t count = session->members[set];
        lhg_module_part *parts = lhg_arena_alloc(&arena, count * sizeof *parts);
        for (size_t at = 0; at < count; at++) {
            parts[at].image = session->overlays[set][at].image;
            parts[at].functions = &session->overlays[set][at].functions;
        }
        lhg_emit_module_set(&arena, exe, session->overlays[set]->index, parts, count, directory, key);
    }
    lhg_ctx_release(&ctx);
    report(&ctx, error, error_size);
    return LHG_OK;
}

int lhg_session_contains(const lhg_session *session, size_t set, unsigned long pc, const unsigned char *bytes, size_t size)
{
    if (set >= session->count)
        return 0;
    size_t count = set ? session->members[set] : 1;
    for (size_t at = 0; at < count; at++) {
        const lhg_image *image = set ? session->overlays[set][at].image : session->program.main;
        unsigned long base = (unsigned long)image->base;
        if (pc < base || pc - base > image->size || size > image->size - (pc - base))
            continue;
        if (!memcmp(image->data + (pc - base), bytes, size))
            return 1;
    }
    return 0;
}

void lhg_session_close(lhg_session *session)
{
    if (!session)
        return;
    lhg_ctx_release(&session->ctx);
    free(session);
}
