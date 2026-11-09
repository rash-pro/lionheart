/* Emits C for a synthetic program description, for the differential test.
 * The description carries raw bytes, so no disc or catalog is involved. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "base.h"
#include "catalog.h"
#include "emit.h"
#include "json.h"
#include "locals.h"
#include "platform.h"

static uint32_t to_u32(lhg_ctx *ctx, const lhg_json *value)
{
    return (uint32_t)lhg_json_int(ctx, value, "number");
}

static const uint8_t *hex_bytes(lhg_arena *arena, const char *text, size_t *size)
{
    size_t length = strlen(text) / 2;
    uint8_t *out = lhg_arena_alloc(arena, length + 1);
    for (size_t index = 0; index < length; index++) {
        unsigned value;
        if (sscanf(text + index * 2, "%2x", &value) != 1)
            lhg_fail(arena->ctx, "bad hex in specification");
        out[index] = (uint8_t)value;
    }
    *size = length;
    return out;
}

static lhg_image *load_image(lhg_arena *arena, const lhg_json *spec)
{
    lhg_ctx *ctx = arena->ctx;
    lhg_image *image = lhg_arena_alloc(arena, sizeof *image);
    memset(image, 0, sizeof *image);
    image->data = hex_bytes(arena, lhg_json_str(ctx, lhg_json_require(ctx, spec, "data", "image"), "data"), &image->size);
    image->base = to_u32(ctx, lhg_json_require(ctx, spec, "base", "image"));
    image->entry = to_u32(ctx, lhg_json_require(ctx, spec, "entry", "image"));
    const lhg_json *ranges = lhg_json_find(spec, "code_ranges");
    if (ranges) {
        lhg_range *list = lhg_arena_alloc(arena, (ranges->as.array.count + 1) * sizeof *list);
        for (size_t index = 0; index < ranges->as.array.count; index++) {
            const lhg_json *pair = ranges->as.array.items[index];
            list[index].low = lhg_json_int(ctx, pair->as.array.items[0], "range");
            list[index].high = lhg_json_int(ctx, pair->as.array.items[1], "range");
        }
        image->code_ranges = list;
        image->code_range_count = ranges->as.array.count;
    }
    const lhg_json *points = lhg_json_find(spec, "entry_points");
    if (points) {
        uint32_t *list = lhg_arena_alloc(arena, (points->as.array.count + 1) * sizeof *list);
        for (size_t index = 0; index < points->as.array.count; index++)
            list[index] = to_u32(ctx, points->as.array.items[index]);
        image->entry_points = list;
        image->entry_point_count = points->as.array.count;
    }
    const lhg_json *meta = lhg_json_find(spec, "metadata");
    points = lhg_json_find(spec, "timing_entry_points");
    if (points) {
        uint32_t *list = lhg_arena_alloc(arena, (points->as.array.count + 1) * sizeof *list);
        for (size_t index = 0; index < points->as.array.count; index++)
            list[index] = to_u32(ctx, points->as.array.items[index]);
        image->timing_entry_points = list;
        image->timing_entry_point_count = points->as.array.count;
    }
    if (meta) {
        lhg_image_meta *info = lhg_arena_alloc(arena, sizeof *info);
        memset(info, 0, sizeof *info);
        info->name = lhg_json_str(ctx, lhg_json_require(ctx, meta, "name", "metadata"), "name");
        info->names = lhg_json_require(ctx, meta, "names", "metadata");
        info->paths = lhg_json_require(ctx, meta, "paths", "metadata");
        const lhg_json *discs = lhg_json_require(ctx, meta, "discs", "metadata");
        for (size_t index = 0; index < discs->as.array.count && index < 4; index++)
            info->discs[info->disc_count++] = (int)lhg_json_int(ctx, discs->as.array.items[index], "disc");
        info->catalog_sha1 = lhg_json_str(ctx, lhg_json_require(ctx, meta, "catalog_sha1", "metadata"), "sha1");
        info->catalog_size = lhg_json_int(ctx, lhg_json_require(ctx, meta, "catalog_size", "metadata"), "size");
        const lhg_json *disc = lhg_json_require(ctx, meta, "disc", "metadata");
        info->has_disc = disc->type != LHG_JSON_NULL;
        info->disc = disc->type == LHG_JSON_NULL ? 0 : (int)lhg_json_int(ctx, disc, "disc");
        info->poll_forward = lhg_json_truth(lhg_json_require(ctx, meta, "poll_forward", "metadata"));
        image->meta = info;
    }
    return image;
}

static void discover_for_spec(lhg_arena *arena, const lhg_image *image, const lhg_json *spec, int experimental, lhg_functions *functions)
{
    lhg_ctx *ctx = arena->ctx;
    const lhg_json *seeds = lhg_json_find(spec, "seeds");
    size_t count = seeds ? seeds->as.array.count : 0;
    size_t timing_count = experimental ? image->timing_entry_point_count : 0;
    uint32_t *list = lhg_arena_alloc(arena, (count + timing_count + 1) * sizeof *list);
    for (size_t index = 0; index < count; index++)
        list[index] = to_u32(ctx, seeds->as.array.items[index]);
    for (size_t index = 0; index < timing_count; index++)
        list[count++] = image->timing_entry_points[index];
    const lhg_json *scan = lhg_json_find(spec, "scan_calls");
    lhg_discover(arena, image, list, count, !scan || lhg_json_truth(scan), functions);
}

static int run(lhg_arena *arena, const char *spec_path, const char *output)
{
    lhg_ctx *ctx = arena->ctx;
    size_t size;
    const char *text = (const char *)lhg_read_file(ctx, spec_path, &size);
    const lhg_json *spec = lhg_json_parse(arena, text, size, spec_path);
    const lhg_json *source = lhg_json_find(spec, "lower_source");
    if (source) {
        int cap = (int)lhg_json_int(ctx, lhg_json_require(ctx, spec, "max_registers", "spec"), "max_registers");
        if (cap < 1 || cap > 8)
            lhg_fail(ctx, "max_registers must be in 1..8");
        lhg_text input, lowered;
        lhg_text_init(&input, ctx);
        lhg_text_init(&lowered, ctx);
        lhg_text_adds(&input, lhg_json_str(ctx, source, "lower_source"));
        lhg_cache_lower(arena, &input, cap, &lowered);
        lhg_make_directories(ctx, output);
        lhg_write_if_changed(ctx, lhg_join_path(arena, output, "lowered.c"), lowered.data, lowered.size);
        lhg_text_free(&input);
        lhg_text_free(&lowered);
        return 0;
    }
    lhg_emit_options options;
    memset(&options, 0, sizeof options);
    const lhg_json *mode = lhg_json_find(spec, "mode");
    options.mode = mode && !strcmp(lhg_json_str(ctx, mode, "mode"), "trace") ? LHG_MODE_TRACE : LHG_MODE_FAST;
    const lhg_json *host = lhg_json_find(spec, "host_mode");
    options.host_mode = host && !strcmp(lhg_json_str(ctx, host, "host_mode"), "fast") ? LHG_HOST_FAST : LHG_HOST_PRECISE;
    const lhg_json *timing = lhg_json_find(spec, "timing");
    if (timing) {
        const char *name = lhg_json_str(ctx, timing, "timing");
        if (!strcmp(name, "block"))
            options.timing = LHG_TIMING_BLOCK;
        else if (!strcmp(name, "loose"))
            options.timing = LHG_TIMING_LOOSE;
        else if (strcmp(name, "exact"))
            lhg_fail(ctx, "unknown timing policy: %s", name);
    }
    if (options.timing != LHG_TIMING_EXACT && options.mode == LHG_MODE_TRACE)
        lhg_fail(ctx, "experimental timing requires fast emission");
    const lhg_json *selection = lhg_json_find(spec, "register_cache");
    if (selection && selection->type != LHG_JSON_NULL)
        options.register_cache = lhg_cache_parse(arena, selection);
    else if (options.timing != LHG_TIMING_EXACT)
        options.register_cache = lhg_cache_read(arena, "games/ff8-usa/play-register-cache.json");
    const lhg_json *exe = lhg_json_find(spec, "exe_path");
    if (exe) {
        lhg_options generate = {0};
        generate.exe_path = lhg_json_str(ctx, exe, "exe_path");
        generate.catalog_dir = lhg_json_str(ctx, lhg_json_require(ctx, spec, "catalog_dir", "spec"), "catalog_dir");
        generate.output_dir = output;
        generate.mode = options.mode;
        generate.host_mode = options.host_mode;
        generate.timing = options.timing;
        generate.register_cache = options.register_cache;
        char error[1024];
        if (lhg_generate(&generate, NULL, error, sizeof error) != LHG_OK)
            lhg_fail(ctx, "%s", error);
        return 0;
    }
    const lhg_json *main_spec = lhg_json_require(ctx, spec, "main", "spec");
    lhg_image *image = load_image(arena, main_spec);
    lhg_functions functions;
    discover_for_spec(arena, image, main_spec, options.timing != LHG_TIMING_EXACT, &functions);
    const lhg_json *overlays = lhg_json_find(spec, "overlays");
    size_t overlay_count = overlays ? overlays->as.array.count : 0;
    lhg_overlay_set *sets = lhg_arena_alloc(arena, (overlay_count + 1) * sizeof *sets);
    for (size_t index = 0; index < overlay_count; index++) {
        const lhg_json *row = overlays->as.array.items[index];
        sets[index].index = (int)lhg_json_int(ctx, lhg_json_require(ctx, row, "index", "overlay"), "index");
        sets[index].image = load_image(arena, row);
        discover_for_spec(arena, sets[index].image, row, 0, &sets[index].functions);
    }
    lhg_emit_result result;
    lhg_emit(arena, image, &functions, output, sets, overlay_count, &options, &result);
    return 0;
}

int main(int argc, char **argv)
{
    if (argc != 3) {
        fprintf(stderr, "usage: lionheart-gen-spec SPEC.json OUTPUT\n");
        return 2;
    }
    lhg_ctx ctx;
    lhg_ctx_init(&ctx);
    lhg_arena arena;
    if (setjmp(ctx.bail)) {
        fprintf(stderr, "spec_emit: %s\n", ctx.error);
        lhg_ctx_release(&ctx);
        return 1;
    }
    lhg_arena_init(&arena, &ctx);
    int result = run(&arena, argv[1], argv[2]);
    lhg_arena_release(&arena);
    lhg_ctx_release(&ctx);
    return result;
}
