/* The module output shape: every overlay set, and the main executable, becomes
 * its own self-contained tree that the host can load at run time.
 *
 * Nothing is shared between trees. A body also needed by another set is written
 * again rather than referenced, because sets arrive and leave independently and
 * the host keeps no record of which other module a borrowed body lives in. The
 * precise host retires through the dispatcher, so bodies never call each other;
 * only a set's own region table is referenced. */
#include "emit.h"

#include <string.h>

#include "platform.h"
#include "sha1.h"

enum { UNIT_FUNCTIONS = 24 };

typedef struct {
    const char *directory, *name;
    int index; /* LH_MODULE_MAIN_SET for the executable. */
    const lhg_image *image;
    const lhg_functions *functions;
} module_set;

static void emit_units(lhg_arena *arena, const module_set *set, lhg_arena *function_arena, lhg_text *body, lhg_text *cmake_sources)
{
    lhg_ctx *ctx = arena->ctx;
    size_t unit_index = 0;
    for (size_t start = 0; start < set->functions->count; start += UNIT_FUNCTIONS, unit_index++) {
        size_t stop = start + UNIT_FUNCTIONS;
        if (stop > set->functions->count)
            stop = set->functions->count;
        char digits[16];
        snprintf(digits, sizeof digits, "%03u", (unsigned)unit_index);
        const char *file = lhg_fmt(arena, "unit_%s.c", digits);
        lhg_text_clear(body);
        lhg_text_adds(body, "#include \"native_module.h\"\n");
        for (size_t row = start; row < stop; row++)
            lhg_text_addf(body, arena, "extern const code_region_t code_%X;\n", set->functions->items[row].entry);
        for (size_t row = start; row < stop; row++) {
            lhg_analysis analysis;
            lhg_analyze(function_arena, &set->functions->items[row], &analysis);
            lhg_emit_precise(function_arena, &analysis, body);
            lhg_arena_reset(function_arena);
        }
        lhg_write_if_changed(ctx, lhg_join_path(arena, set->directory, file), body->data, body->size);
        lhg_text_addf(cmake_sources, arena, "  \"${CMAKE_CURRENT_LIST_DIR}/modules/%s/%s\"\n", set->name, file);
    }
}

static size_t emit_tables(lhg_arena *arena, const module_set *set, const lhg_image *exe, lhg_arena *scratch, lhg_text *tables, lhg_text *cmake_sources)
{
    lhg_ctx *ctx = arena->ctx;
    const lhg_functions *functions = set->functions;
    lhg_text_clear(tables);
    lhg_text_adds(tables, "#include \"native_module.h\"\nlh_module_api lh_api;\n");
    for (size_t row = 0; row < functions->count; row++) {
        const lhg_function *f = &functions->items[row];
        lhg_text_addf(tables, scratch, "void f_%X(cpu_t *c);\n", f->entry);
        lhg_span_list spans;
        lhg_code_spans(scratch, f, &spans);
        uint64_t fingerprint = lhg_spans_fingerprint(set->image, &spans);
        lhg_text_addf(tables, scratch, "static const code_span_t code_%X_spans[] = {", f->entry);
        for (uint32_t s = 0; s < spans.count; s++) {
            if (s)
                lhg_text_adds(tables, ",");
            lhg_text_addf(tables, scratch, "{%U, %d}", spans.items[s].start, (int)spans.items[s].size);
        }
        /* The precise host never reads the event gap and windows; those
         * belong to the fast variants, which modules do not contain. */
        lhg_text_addf(tables, scratch, "};\nconst code_region_t code_%X = {code_%X_spans, %d, UINT64_C(%Q), 0, NULL};\n", f->entry, f->entry,
                      (int)spans.count, fingerprint);
        lhg_arena_reset(scratch);
    }
    lhg_entry_pair *pairs;
    size_t count = lhg_build_entry_map(ctx, functions, scratch, 0, &pairs);
    lhg_arena_reset(scratch);
    if (count) {
        lhg_text_adds(tables, "static const entry_t module_entries[] = {\n");
        for (size_t index = 0; index < count; index++)
            lhg_text_addf(tables, scratch, "{%U, f_%X},\n", pairs[index].pc, pairs[index].owner);
        lhg_text_adds(tables, "};\nstatic const code_region_t *const module_regions[] = {\n");
        for (size_t index = 0; index < count; index++)
            lhg_text_addf(tables, scratch, "&code_%X,\n", pairs[index].owner);
        lhg_text_adds(tables, "};\n");
        lhg_arena_reset(scratch);
    }
    lhg_free(ctx, pairs);

    uint8_t digest[20];
    lhg_sha1(set->image->data, set->image->size, digest);
    lhg_text_addf(tables, scratch, "LH_MODULE_EXPORT const lh_module_descriptor_t lh_module_descriptor = {\n"
                  "LH_MODULE_MAGIC, LH_MODULE_ABI_VERSION, (uint32_t)sizeof(lh_module_descriptor_t), LH_MODULE_ABI_ID,\n"
                  "LH_MODULE_TRAITS_INIT, %d, %U, %D, {", set->index, set->image->base, (int64_t)set->image->size);
    for (int byte = 0; byte < 20; byte++) {
        if (byte)
            lhg_text_adds(tables, ",");
        lhg_text_addf(tables, scratch, "%d", (int)digest[byte]);
    }
    lhg_text_addf(tables, scratch, "},\n%U, %U, %D,\n", lhg_fnv1a(exe->data, exe->size), exe->base, (int64_t)exe->size);
    if (count)
        lhg_text_addf(tables, scratch, "module_entries, %D, module_regions,\n", (int64_t)count);
    else
        lhg_text_adds(tables, "NULL, 0, NULL,\n");
    lhg_text_adds(tables, "NULL, 0, &lh_api\n};\n");
    lhg_arena_reset(scratch);
    lhg_write_if_changed(ctx, lhg_join_path(arena, set->directory, "tables.c"), tables->data, tables->size);
    lhg_text_addf(cmake_sources, arena, "  \"${CMAKE_CURRENT_LIST_DIR}/modules/%s/tables.c\"\n", set->name);
    return count;
}

/* Lists every instruction once, counting those shared between a set's functions once. */
static size_t set_instructions(lhg_ctx *ctx, const lhg_functions *functions)
{
    uint8_t *seen = lhg_zalloc(ctx, 0x200000 / 4 + 1, 1);
    size_t unique = 0;
    for (size_t row = 0; row < functions->count; row++) {
        const lhg_function *f = &functions->items[row];
        for (uint32_t i = 0; i < f->insn_count; i++) {
            uint32_t word = (f->insns[i].pc & 0x1fffffu) >> 2;
            if (!seen[word]) {
                seen[word] = 1;
                unique++;
            }
        }
    }
    lhg_free(ctx, seen);
    return unique;
}

static size_t emit_module(lhg_arena *arena, const module_set *set, const lhg_image *exe, lhg_arena *function_arena, lhg_text *body,
                          lhg_text *tables, lhg_text *cmake, lhg_json *rows, lhg_arena *json_arena)
{
    lhg_ctx *ctx = arena->ctx;
    lhg_make_directories(ctx, set->directory);
    lhg_text sources;
    lhg_text_init(&sources, ctx);
    emit_units(arena, set, function_arena, body, &sources);
    size_t entries = emit_tables(arena, set, exe, function_arena, tables, &sources);
    lhg_text_addf(cmake, arena, "set(LH_MODULE_SOURCES_%s\n", set->name);
    lhg_text_add(cmake, sources.data, sources.size);
    lhg_text_adds(cmake, ")\n");
    lhg_text_free(&sources);

    char sha1[41];
    lhg_sha1_hex(set->image->data, set->image->size, sha1);
    lhg_json *row = lhg_json_new_object(json_arena);
    lhg_json_set(json_arena, row, "name", lhg_json_new_str(json_arena, set->name));
    lhg_json_set(json_arena, row, "index", lhg_json_new_int(json_arena, set->index));
    lhg_json_set(json_arena, row, "base", lhg_json_new_str(json_arena, lhg_fmt(json_arena, "%H", (uint64_t)set->image->base)));
    lhg_json_set(json_arena, row, "size", lhg_json_new_int(json_arena, (int64_t)set->image->size));
    lhg_json_set(json_arena, row, "sha1", lhg_json_new_str(json_arena, sha1));
    lhg_json_set(json_arena, row, "functions", lhg_json_new_int(json_arena, (int64_t)set->functions->count));
    size_t instructions = set_instructions(ctx, set->functions);
    lhg_json_set(json_arena, row, "instructions", lhg_json_new_int(json_arena, (int64_t)instructions));
    lhg_json_set(json_arena, row, "dispatch_entries", lhg_json_new_int(json_arena, (int64_t)entries));
    lhg_json_append(json_arena, rows, row);
    lhg_arena_reset(arena);
    return instructions;
}

static void emit_host_support(lhg_arena *arena, const lhg_image *image, const char *output, lhg_arena *json_arena, lhg_json *rows)
{
    lhg_ctx *ctx = arena->ctx;
    uint32_t hash = lhg_fnv1a(image->data, image->size);
    lhg_text text;
    lhg_text_init(&text, ctx);
    lhg_text_adds(&text, "#ifndef FF8_GENERATED_H\n#define FF8_GENERATED_H\n#define GENERATED_HAS_OVERLAYS 1\n#include \"cpu.h\"\n#include \"gte.h\"\n"
                  "\nextern const entry_t generated_entries[];\nextern const size_t generated_count;\n"
                  "extern const code_region_t *const generated_regions[];\nextern const overlay_t generated_overlays[];\nextern const size_t generated_overlay_count;\n"
                  "extern const uint32_t generated_hash, generated_base, generated_size;\n#endif\n");
    lhg_write_if_changed(ctx, lhg_join_path(arena, output, "generated.h"), text.data, text.size);

    /* The host linked against this tree holds no game code; its modules
     * arrive at run time, so the tables start out empty. */
    lhg_text_clear(&text);
    lhg_text_adds(&text, "#include \"generated.h\"\n"
                  "const entry_t generated_entries[1] = {{0, 0}};\nconst size_t generated_count = 0;\n"
                  "const code_region_t *const generated_regions[1] = {0};\n"
                  "const overlay_t generated_overlays[1] = {{0}};\nconst size_t generated_overlay_count = 0;\n"
                  "const unsigned generated_host_precise = 1;\n");
    lhg_text_addf(&text, arena, "const uint32_t generated_hash = %U;\nconst uint32_t generated_base = %U, generated_size = %D;\n", hash, image->base,
                  (int64_t)image->size);
    lhg_write_if_changed(ctx, lhg_join_path(arena, output, "dispatch_table.c"), text.data, text.size);

    lhg_text_clear(&text);
    lhg_text_adds(&text, "set(GENERATED_SOURCES\n  \"${CMAKE_CURRENT_LIST_DIR}/dispatch_table.c\"\n)\n");
    lhg_write_if_changed(ctx, lhg_join_path(arena, output, "sources.cmake"), text.data, text.size);

    lhg_json *manifest = lhg_json_new_object(json_arena);
    lhg_json_set(json_arena, manifest, "mode", lhg_json_new_str(json_arena, "fast"));
    lhg_json_set(json_arena, manifest, "host_mode", lhg_json_new_str(json_arena, "precise"));
    lhg_json_set(json_arena, manifest, "shape", lhg_json_new_str(json_arena, "modules"));
    lhg_json_set(json_arena, manifest, "base", lhg_json_new_str(json_arena, lhg_fmt(json_arena, "%H", (uint64_t)image->base)));
    lhg_json_set(json_arena, manifest, "size", lhg_json_new_int(json_arena, (int64_t)image->size));
    lhg_json_set(json_arena, manifest, "fnv1a", lhg_json_new_str(json_arena, lhg_fmt(json_arena, "%H", (uint64_t)hash)));
    lhg_json_set(json_arena, manifest, "modules", rows);
    lhg_text_clear(&text);
    lhg_json_dump(&text, manifest);
    lhg_text_adds(&text, "\n");
    lhg_write_if_changed(ctx, lhg_join_path(arena, output, "manifest.json"), text.data, text.size);
    lhg_text_free(&text);
}

void lhg_emit_modules(lhg_arena *arena, const lhg_image *image, const lhg_functions *functions, const char *output,
                      const lhg_overlay_set *overlays, size_t overlay_count, lhg_emit_result *result)
{
    lhg_ctx *ctx = arena->ctx;
    lhg_make_directories(ctx, output);
    const char *root = lhg_join_path(arena, output, "modules");
    lhg_make_directories(ctx, root);

    lhg_arena unit_arena, function_arena, json_arena;
    lhg_arena_init(&unit_arena, ctx);
    lhg_arena_init(&function_arena, ctx);
    lhg_arena_init(&json_arena, ctx);
    lhg_text body, tables, cmake;
    lhg_text_init(&body, ctx);
    lhg_text_init(&tables, ctx);
    lhg_text_init(&cmake, ctx);
    lhg_json *rows = lhg_json_new_array(&json_arena);
    lhg_text_adds(&cmake, "set(LH_MODULES main");
    for (size_t index = 0; index < overlay_count; index++)
        lhg_text_addf(&cmake, &unit_arena, " set-%d", overlays[index].index);
    lhg_text_adds(&cmake, ")\n");
    lhg_arena_reset(&unit_arena);

    size_t instructions = 0;
    for (size_t index = 0; index <= overlay_count; index++) {
        module_set set;
        if (!index) {
            set.name = "main";
            set.index = -1;
            set.image = image;
            set.functions = functions;
        } else {
            const lhg_overlay_set *overlay = &overlays[index - 1];
            set.name = lhg_fmt(&unit_arena, "set-%d", overlay->index);
            set.index = overlay->index;
            set.image = overlay->image;
            set.functions = &overlay->functions;
        }
        set.directory = lhg_join_path(&unit_arena, root, set.name);
        instructions += emit_module(&unit_arena, &set, image, &function_arena, &body, &tables, &cmake, rows, &json_arena);
        lhg_progress(ctx, "emit", (unsigned long)(index + 1), (unsigned long)(overlay_count + 1));
    }
    lhg_write_if_changed(ctx, lhg_join_path(&unit_arena, output, "modules.cmake"), cmake.data, cmake.size);
    emit_host_support(&unit_arena, image, output, &json_arena, rows);

    result->functions = functions->count;
    /* Summed per module: replication is what this shape costs, so it is counted. */
    result->instructions = instructions;
    result->jump_tables = 0;
    lhg_text_free(&body);
    lhg_text_free(&tables);
    lhg_text_free(&cmake);
    lhg_arena_release(&unit_arena);
    lhg_arena_release(&function_arena);
    lhg_arena_release(&json_arena);
}
