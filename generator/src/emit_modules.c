/* The module output shape: every overlay set, and the main executable, becomes
 * its own self-contained tree that the host can load at run time. The variants
 * of one code prefix are one set to the host and become one tree.
 *
 * Nothing is shared between trees. A body also needed by another set is written
 * again rather than referenced, because sets arrive and leave independently and
 * the host keeps no record of which other module a borrowed body lives in. The
 * precise host retires through the dispatcher, so bodies never call each other;
 * only a set's own region table is referenced. */
#include "emit.h"

#include <stdlib.h>
#include <string.h>

#include "platform.h"
#include "sha1.h"
#include "sha256.h"


enum { UNIT_FUNCTIONS = 24 };

/* One body of a module, with the image its fingerprint reads. */
typedef struct {
    const lhg_function *function;
    const lhg_image *image;
    const char *suffix;
} module_function;

typedef struct {
    const char *directory, *name;
    int index; /* LH_MODULE_MAIN_SET for the executable. */
    const lhg_module_part *parts;
    size_t part_count;
    /* Every distinct body once, and for each part the row of each of its functions. */
    module_function *functions;
    size_t function_count;
    uint32_t **rows;
    uint32_t base, size;
    uint8_t sha1[20];
    /* NULL directory: emit nowhere. A key, when present, sees every byte that
     * shapes the module's code but not which set it was emitted for. */
    lhg_sha256 *key;
} module_set;

static int same_body(lhg_arena *arena, const module_function *known, const lhg_function *f, const lhg_image *image)
{
    if (known->function->entry != f->entry)
        return 0;
    lhg_span_list a, b;
    lhg_code_spans(arena, known->function, &a);
    lhg_code_spans(arena, f, &b);
    if (a.count != b.count)
        return 0;
    for (uint32_t at = 0; at < a.count; at++)
        if (a.items[at].start != b.items[at].start || a.items[at].size != b.items[at].size ||
            memcmp(lhg_image_bytes(known->image, a.items[at].start), lhg_image_bytes(image, b.items[at].start), a.items[at].size))
            return 0;
    return 1;
}

/* The variants of a code prefix share most functions; each distinct body is
 * written once, and a second body at the same address gets a numbered name.
 * A module of one image keeps its functions, names and order exactly. */
static void prepare(lhg_arena *arena, module_set *set, const lhg_module_part *parts, size_t part_count)
{
    lhg_ctx *ctx = arena->ctx;
    size_t total = 0;
    for (size_t part = 0; part < part_count; part++)
        total += parts[part].functions->count;
    set->parts = parts;
    set->part_count = part_count;
    set->functions = lhg_arena_alloc(arena, (total + 1) * sizeof *set->functions);
    set->function_count = 0;
    set->rows = lhg_arena_alloc(arena, part_count * sizeof *set->rows);
    set->base = parts[0].image->base;
    set->size = 0;
    for (size_t part = 0; part < part_count; part++) {
        const lhg_image *image = parts[part].image;
        if (image->base != set->base)
            lhg_fail(ctx, "internal error: the images of one module start at different addresses");
        if (image->size > set->size)
            set->size = (uint32_t)image->size;
        set->rows[part] = lhg_arena_alloc(arena, (parts[part].functions->count + 1) * sizeof **set->rows);
        for (size_t row = 0; row < parts[part].functions->count; row++) {
            const lhg_function *f = &parts[part].functions->items[row];
            size_t found = set->function_count, same_entry = 0;
            for (size_t known = 0; part && known < set->function_count; known++) {
                if (set->functions[known].function->entry != f->entry)
                    continue;
                if (same_body(arena, &set->functions[known], f, image)) {
                    found = known;
                    break;
                }
                same_entry++;
            }
            if (found == set->function_count) {
                set->functions[found].function = f;
                set->functions[found].image = image;
                set->functions[found].suffix = same_entry ? lhg_fmt(arena, "_%d", (int)same_entry) : "";
                set->function_count++;
            }
            set->rows[part][row] = (uint32_t)found;
        }
    }
    if (part_count == 1) {
        lhg_sha1(parts[0].image->data, parts[0].image->size, set->sha1);
        return;
    }
    /* No single image: the digest of all of them, in variant order. */
    size_t bytes = 0;
    for (size_t part = 0; part < part_count; part++)
        bytes += parts[part].image->size;
    uint8_t *joined = lhg_alloc(ctx, bytes + 1);
    bytes = 0;
    for (size_t part = 0; part < part_count; part++) {
        memcpy(joined + bytes, parts[part].image->data, parts[part].image->size);
        bytes += parts[part].image->size;
    }
    lhg_sha1(joined, bytes, set->sha1);
    lhg_free(ctx, joined);
}

static void put_file(lhg_ctx *ctx, lhg_arena *arena, const module_set *set, const char *file, const lhg_text *text, size_t keyed)
{
    if (set->key) {
        char header[96];
        snprintf(header, sizeof header, "%s %llu\n", file, (unsigned long long)keyed);
        lhg_sha256_add(set->key, header, strlen(header));
        lhg_sha256_add(set->key, text->data, keyed);
    }
    if (set->directory)
        lhg_write_if_changed(ctx, lhg_join_path(arena, set->directory, file), text->data, text->size);
}

static void emit_units(lhg_arena *arena, const module_set *set, lhg_arena *function_arena, lhg_text *body, lhg_text *cmake_sources)
{
    lhg_ctx *ctx = arena->ctx;
    size_t unit_index = 0;
    for (size_t start = 0; start < set->function_count; start += UNIT_FUNCTIONS, unit_index++) {
        size_t stop = start + UNIT_FUNCTIONS;
        if (stop > set->function_count)
            stop = set->function_count;
        char digits[16];
        snprintf(digits, sizeof digits, "%03u", (unsigned)unit_index);
        const char *file = lhg_fmt(arena, "unit_%s.c", digits);
        lhg_text_clear(body);
        lhg_text_adds(body, "#include \"native_module.h\"\n");
        for (size_t row = start; row < stop; row++)
            lhg_text_addf(body, arena, "extern const code_region_t code_%X%s;\n", set->functions[row].function->entry, set->functions[row].suffix);
        for (size_t row = start; row < stop; row++) {
            lhg_analysis analysis;
            lhg_analyze(function_arena, set->functions[row].function, &analysis);
            lhg_emit_precise(function_arena, &analysis, set->functions[row].suffix, body);
            lhg_arena_reset(function_arena);
        }
        put_file(ctx, arena, set, file, body, body->size);
        if (cmake_sources)
            lhg_text_addf(cmake_sources, arena, "  \"${CMAKE_CURRENT_LIST_DIR}/modules/%s/%s\"\n", set->name, file);
    }
}

typedef struct {
    uint32_t pc, function, sequence;
} module_entry;

static int compare_module_entry(const void *a, const void *b)
{
    const module_entry *x = a, *y = b;
    if (x->pc != y->pc)
        return x->pc < y->pc ? -1 : 1;
    return x->sequence < y->sequence ? -1 : x->sequence > y->sequence;
}

/* Every part's resume points, by address. Where variants differ, one address
 * has several entries, in variant order; the precise host tries each until a
 * fingerprint holds, as it does for overlapping sets. */
static size_t module_entries(lhg_ctx *ctx, const module_set *set, lhg_arena *scratch, module_entry **out)
{
    size_t count = 0, capacity = 16;
    module_entry *entries = lhg_alloc(ctx, capacity * sizeof *entries);
    for (size_t part = 0; part < set->part_count; part++) {
        const lhg_functions *functions = set->parts[part].functions;
        lhg_entry_pair *pairs;
        size_t pair_count = lhg_build_entry_map(ctx, functions, scratch, 0, &pairs);
        lhg_arena_reset(scratch);
        for (size_t at = 0; at < pair_count; at++) {
            const lhg_function *owner = lhg_functions_find(functions, pairs[at].owner);
            entries = lhg_grow(ctx, entries, &capacity, count, sizeof *entries);
            entries[count].pc = pairs[at].pc;
            entries[count].function = set->rows[part][owner - functions->items];
            entries[count].sequence = (uint32_t)count;
            count++;
        }
        lhg_free(ctx, pairs);
    }
    qsort(entries, count, sizeof *entries, compare_module_entry);
    size_t unique = 0;
    for (size_t at = 0; at < count; at++) {
        size_t seen = unique;
        while (seen > 0 && entries[seen - 1].pc == entries[at].pc && entries[seen - 1].function != entries[at].function)
            seen--;
        if (seen > 0 && entries[seen - 1].pc == entries[at].pc)
            continue;
        entries[unique++] = entries[at];
    }
    *out = entries;
    return unique;
}

static size_t emit_tables(lhg_arena *arena, const module_set *set, const lhg_image *exe, lhg_arena *scratch, lhg_text *tables, lhg_text *cmake_sources)
{
    lhg_ctx *ctx = arena->ctx;
    lhg_text_clear(tables);
    lhg_text_adds(tables, "#include \"native_module.h\"\nlh_module_api lh_api;\n");
    for (size_t row = 0; row < set->function_count; row++) {
        const lhg_function *f = set->functions[row].function;
        const char *suffix = set->functions[row].suffix;
        lhg_text_addf(tables, scratch, "void f_%X%s(cpu_t *c);\n", f->entry, suffix);
        lhg_span_list spans;
        lhg_code_spans(scratch, f, &spans);
        uint64_t fingerprint = lhg_spans_fingerprint(set->functions[row].image, &spans);
        lhg_text_addf(tables, scratch, "static const code_span_t code_%X%s_spans[] = {", f->entry, suffix);
        for (uint32_t s = 0; s < spans.count; s++) {
            if (s)
                lhg_text_adds(tables, ",");
            lhg_text_addf(tables, scratch, "{%U, %d}", spans.items[s].start, (int)spans.items[s].size);
        }
        /* The precise host never reads the event gap and windows; those
         * belong to the fast variants, which modules do not contain. */
        lhg_text_addf(tables, scratch, "};\nconst code_region_t code_%X%s = {code_%X%s_spans, %d, UINT64_C(%Q), 0, NULL};\n", f->entry, suffix,
                      f->entry, suffix, (int)spans.count, fingerprint);
        lhg_arena_reset(scratch);
    }
    module_entry *entries;
    size_t count = module_entries(ctx, set, scratch, &entries);
    if (count) {
        lhg_text_adds(tables, "static const entry_t module_entries[] = {\n");
        for (size_t index = 0; index < count; index++) {
            const module_function *owner = &set->functions[entries[index].function];
            lhg_text_addf(tables, scratch, "{%U, f_%X%s},\n", entries[index].pc, owner->function->entry, owner->suffix);
        }
        lhg_text_adds(tables, "};\nstatic const code_region_t *const module_regions[] = {\n");
        for (size_t index = 0; index < count; index++) {
            const module_function *owner = &set->functions[entries[index].function];
            lhg_text_addf(tables, scratch, "&code_%X%s,\n", owner->function->entry, owner->suffix);
        }
        lhg_text_adds(tables, "};\n");
        lhg_arena_reset(scratch);
    }
    lhg_free(ctx, entries);

    /* Everything above is the code; the descriptor below also names the set. */
    size_t keyed = tables->size;
    if (set->key) {
        char facts[160];
        snprintf(facts, sizeof facts, "main %d base %lu size %lu exe %lu %lu %lu entries %lu\n", set->index == -1,
                 (unsigned long)set->base, (unsigned long)set->size, (unsigned long)lhg_fnv1a(exe->data, exe->size),
                 (unsigned long)exe->base, (unsigned long)exe->size, (unsigned long)count);
        lhg_sha256_add(set->key, facts, strlen(facts));
    }
    lhg_text_addf(tables, scratch, "LH_MODULE_EXPORT const lh_module_descriptor_t lh_module_descriptor = {\n"
                  "LH_MODULE_MAGIC, LH_MODULE_ABI_VERSION, (uint32_t)sizeof(lh_module_descriptor_t), LH_MODULE_ABI_ID,\n"
                  "LH_MODULE_TRAITS_INIT, %d, %U, %D, {", set->index, set->base, (int64_t)set->size);
    for (int byte = 0; byte < 20; byte++) {
        if (byte)
            lhg_text_adds(tables, ",");
        lhg_text_addf(tables, scratch, "%d", (int)set->sha1[byte]);
    }
    lhg_text_addf(tables, scratch, "},\n%U, %U, %D,\n", lhg_fnv1a(exe->data, exe->size), exe->base, (int64_t)exe->size);
    if (count)
        lhg_text_addf(tables, scratch, "module_entries, %D, module_regions,\n", (int64_t)count);
    else
        lhg_text_adds(tables, "NULL, 0, NULL,\n");
    lhg_text_adds(tables, "NULL, 0, &lh_api\n};\n");
    lhg_arena_reset(scratch);
    put_file(ctx, arena, set, "tables.c", tables, keyed);
    if (cmake_sources)
        lhg_text_addf(cmake_sources, arena, "  \"${CMAKE_CURRENT_LIST_DIR}/modules/%s/tables.c\"\n", set->name);
    return count;
}

/* Lists every instruction once, counting those shared between a set's functions once. */
static size_t set_instructions(lhg_ctx *ctx, const module_set *set)
{
    uint8_t *seen = lhg_zalloc(ctx, 0x200000 / 4 + 1, 1);
    size_t unique = 0;
    for (size_t row = 0; row < set->function_count; row++) {
        const lhg_function *f = set->functions[row].function;
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
    for (int byte = 0; byte < 20; byte++)
        snprintf(sha1 + byte * 2, 3, "%02x", set->sha1[byte]);
    lhg_json *row = lhg_json_new_object(json_arena);
    lhg_json_set(json_arena, row, "name", lhg_json_new_str(json_arena, set->name));
    lhg_json_set(json_arena, row, "index", lhg_json_new_int(json_arena, set->index));
    lhg_json_set(json_arena, row, "base", lhg_json_new_str(json_arena, lhg_fmt(json_arena, "%H", (uint64_t)set->base)));
    lhg_json_set(json_arena, row, "size", lhg_json_new_int(json_arena, (int64_t)set->size));
    lhg_json_set(json_arena, row, "sha1", lhg_json_new_str(json_arena, sha1));
    lhg_json_set(json_arena, row, "functions", lhg_json_new_int(json_arena, (int64_t)set->function_count));
    size_t instructions = set_instructions(ctx, set);
    lhg_json_set(json_arena, row, "instructions", lhg_json_new_int(json_arena, (int64_t)instructions));
    lhg_json_set(json_arena, row, "dispatch_entries", lhg_json_new_int(json_arena, (int64_t)entries));
    if (set->part_count > 1)
        lhg_json_set(json_arena, row, "variants", lhg_json_new_int(json_arena, (int64_t)set->part_count));
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
        if (!overlays[index].shares_row)
            lhg_text_addf(&cmake, &unit_arena, " set-%d", overlays[index].index);
    lhg_text_adds(&cmake, ")\n");
    lhg_arena_reset(&unit_arena);

    size_t instructions = 0;
    for (size_t index = 0; index <= overlay_count;) {
        module_set set;
        memset(&set, 0, sizeof set);
        lhg_module_part *parts;
        size_t count = 1;
        if (!index) {
            set.name = "main";
            set.index = -1;
            parts = lhg_arena_alloc(&unit_arena, sizeof *parts);
            parts[0].image = image;
            parts[0].functions = functions;
        } else {
            const lhg_overlay_set *overlay = &overlays[index - 1];
            while (index - 1 + count < overlay_count && overlay[count].shares_row)
                count++;
            set.name = lhg_fmt(&unit_arena, "set-%d", overlay->index);
            set.index = overlay->index;
            parts = lhg_arena_alloc(&unit_arena, count * sizeof *parts);
            for (size_t at = 0; at < count; at++) {
                parts[at].image = overlay[at].image;
                parts[at].functions = &overlay[at].functions;
            }
        }
        prepare(&unit_arena, &set, parts, count);
        set.directory = lhg_join_path(&unit_arena, root, set.name);
        instructions += emit_module(&unit_arena, &set, image, &function_arena, &body, &tables, &cmake, rows, &json_arena);
        index += count;
        lhg_progress(ctx, "emit", (unsigned long)index, (unsigned long)(overlay_count + 1));
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

void lhg_emit_module_set(lhg_arena *arena, const lhg_image *exe, int index, const lhg_module_part *parts, size_t part_count,
                         const char *directory, char key[65])
{
    lhg_ctx *ctx = arena->ctx;
    lhg_sha256 hash;
    lhg_sha256_init(&hash);
    lhg_sha256_add(&hash, "lionheart-module-1\n", 19);
    module_set set;
    memset(&set, 0, sizeof set);
    set.directory = directory;
    set.name = index == -1 ? "main" : lhg_fmt(arena, "set-%d", index);
    set.index = index;
    prepare(arena, &set, parts, part_count);
    set.key = &hash;
    lhg_arena function_arena;
    lhg_arena_init(&function_arena, ctx);
    lhg_text body, tables;
    lhg_text_init(&body, ctx);
    lhg_text_init(&tables, ctx);
    emit_units(arena, &set, &function_arena, &body, NULL);
    emit_tables(arena, &set, exe, &function_arena, &tables, NULL);
    lhg_sha256_finish(&hash, key);
    lhg_text_free(&body);
    lhg_text_free(&tables);
    lhg_arena_release(&function_arena);
}
