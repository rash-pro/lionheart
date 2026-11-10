#include "catalog.h"

#include <stdlib.h>
#include <string.h>

#include "disc.h"
#include "platform.h"
#include "sha1.h"
#include "sha256.h"

/* Loader entry points of the overlays the executable launches. */
static uint32_t entry_for_top(int top, uint32_t base)
{
    switch (top) {
    case 0: return 0x80098028;
    case 1: return 0x80098FD4;
    case 2: return 0x8009895C;
    case 23: return 0x80098304;
    case 24: return 0x80098238;
    case 25: return 0x80099D30;
    default: return base;
    }
}

static int in_entry_table(int top)
{
    return top == 0 || top == 1 || top == 2 || top == 23 || top == 24 || top == 25;
}

static int is_legacy_top(int top)
{
    return in_entry_table(top) || (top >= 4 && top <= 20) || top == 26;
}

static uint32_t read_u32(const uint8_t *at)
{
    return (uint32_t)at[0] | (uint32_t)at[1] << 8 | (uint32_t)at[2] << 16 | (uint32_t)at[3] << 24;
}

static const lhg_json *load_json(lhg_arena *arena, const char *directory, const char *name)
{
    lhg_ctx *ctx = arena->ctx;
    for (size_t index = 0; index < ctx->catalog_file_count; index++)
        if (!strcmp(ctx->catalog_files[index].name, name)) {
            /* The parser keeps pointers into its text, so it gets an arena copy. */
            char *copy = lhg_arena_strdup(arena, (const char *)ctx->catalog_files[index].data, ctx->catalog_files[index].size);
            return lhg_json_parse(arena, copy, ctx->catalog_files[index].size, name);
        }
    if (ctx->catalog_file_count)
        lhg_fail(ctx, "the embedded catalog has no %s", name);
    const char *path = lhg_join_path(arena, directory, name);
    size_t size;
    char *data = (char *)lhg_read_file(arena->ctx, path, &size);
    const lhg_json *root = lhg_json_parse(arena, data, size, path);
    lhg_free(arena->ctx, data);
    return root;
}

static uint32_t parse_hex32(lhg_ctx *ctx, const char *text, const char *where)
{
    if (text[0] == '0' && (text[1] == 'x' || text[1] == 'X'))
        text += 2;
    if (!*text)
        lhg_fail(ctx, "%s: expected a hexadecimal number", where);
    uint64_t value = 0;
    for (; *text; text++) {
        unsigned digit;
        if (*text >= '0' && *text <= '9')
            digit = (unsigned)(*text - '0');
        else if (*text >= 'a' && *text <= 'f')
            digit = (unsigned)(*text - 'a' + 10);
        else if (*text >= 'A' && *text <= 'F')
            digit = (unsigned)(*text - 'A' + 10);
        else
            lhg_fail(ctx, "%s: expected a hexadecimal number", where);
        value = value * 16 + digit;
        if (value > UINT32_MAX)
            lhg_fail(ctx, "%s: address out of range", where);
    }
    return (uint32_t)value;
}

/* Reads a PS-X EXE header. data starts at the EXE's first byte. */
static lhg_image *parse_exe(lhg_arena *arena, const uint8_t *data, size_t size)
{
    lhg_ctx *ctx = arena->ctx;
    if (size < 0x800 || memcmp(data, "PS-X EXE", 8))
        lhg_fail(ctx, "not a PS-X EXE");
    uint32_t entry = read_u32(data + 0x10), base = read_u32(data + 0x18), text = read_u32(data + 0x1C);
    if ((size_t)text > size - 0x800 || (uint64_t)(base & 0x1FFFFFFF) + text > 0x200000)
        lhg_fail(ctx, "truncated EXE or text outside RAM");
    if (base & 3 || text & 3 || entry & 3 || entry < base || (uint64_t)entry >= (uint64_t)base + text)
        lhg_fail(ctx, "unaligned EXE or entry outside text");
    lhg_image *image = lhg_arena_alloc(arena, sizeof *image);
    memset(image, 0, sizeof *image);
    image->data = data + 0x800;
    image->size = text;
    image->base = base;
    image->entry = entry;
    return image;
}

/* Applies the versioned identity and code ranges to the main executable. */
static void apply_exe_metadata(lhg_arena *arena, lhg_image *image, const lhg_json *metadata,
                               const uint8_t *exe, size_t exe_size)
{
    lhg_ctx *ctx = arena->ctx;
    char sha1[41];
    lhg_sha1_hex(exe, exe_size, sha1);
    if (strcmp(sha1, lhg_json_str(ctx, lhg_json_require(ctx, metadata, "exe_sha1", "overlays.json"), "exe_sha1")))
        lhg_fail(ctx, "unsupported EXE SHA-1; FF8 USA metadata required");
    const lhg_json *ranges = lhg_json_list(ctx, lhg_json_require(ctx, metadata, "main_code_ranges", "overlays.json"), "main_code_ranges");
    size_t range_count = ranges->as.array.count;
    lhg_range *list = lhg_arena_alloc(arena, (range_count + 1) * sizeof *list);
    for (size_t index = 0; index < range_count; index++) {
        const lhg_json *pair = lhg_json_list(ctx, ranges->as.array.items[index], "main_code_ranges row");
        if (pair->as.array.count != 2)
            lhg_fail(ctx, "main_code_ranges row must have two values");
        list[index].low = (int64_t)parse_hex32(ctx, lhg_json_str(ctx, pair->as.array.items[0], "range low"), "range low");
        list[index].high = (int64_t)parse_hex32(ctx, lhg_json_str(ctx, pair->as.array.items[1], "range high"), "range high");
    }
    image->code_ranges = list;
    image->code_range_count = range_count;
    const lhg_json *points = lhg_json_find(metadata, "main_entry_points");
    if (points) {
        lhg_json_list(ctx, points, "main_entry_points");
        size_t count = points->as.array.count;
        uint32_t *entries = lhg_arena_alloc(arena, (count + 1) * sizeof *entries);
        for (size_t index = 0; index < count; index++)
            entries[index] = parse_hex32(ctx, lhg_json_str(ctx, points->as.array.items[index], "main_entry_points"), "main_entry_points");
        image->entry_points = entries;
        image->entry_point_count = count;
    }
    for (size_t index = 0; index < image->entry_point_count; index++)
        if (!lhg_image_executable(image, (int64_t)image->entry_points[index]))
            lhg_fail(ctx, "main entry point outside executable ranges");
    points = lhg_json_find(metadata, "timing_entry_points");
    if (points) {
        lhg_json_list(ctx, points, "timing_entry_points");
        size_t count = points->as.array.count;
        uint32_t *entries = lhg_arena_alloc(arena, (count + 1) * sizeof *entries);
        for (size_t index = 0; index < count; index++)
            entries[index] = parse_hex32(ctx, lhg_json_str(ctx, points->as.array.items[index], "timing_entry_points"), "timing_entry_points");
        image->timing_entry_points = entries;
        image->timing_entry_point_count = count;
    }
}

void lhg_load_exe(lhg_arena *arena, const char *catalog_dir, const char *path, lhg_program *program)
{
    lhg_ctx *ctx = arena->ctx;
    size_t size;
    const uint8_t *data = lhg_read_file(ctx, path, &size);
    lhg_image *image = parse_exe(arena, data, size);
    apply_exe_metadata(arena, image, load_json(arena, catalog_dir, "overlays.json"), data, size);
    memset(program, 0, sizeof *program);
    program->main = image;
}

static const lhg_disc_file *lookup_exact(lhg_disc *disc, const char *name)
{
    /* Later duplicates replace earlier ones, so the search runs from the end. */
    for (size_t index = disc->file_count; index > 0; index--)
        if (!strcmp(disc->files[index - 1].path + 1, name))
            return &disc->files[index - 1];
    return NULL;
}

static int compare_set(const void *a, const void *b)
{
    int x = ((const lhg_overlay_set *)a)->index, y = ((const lhg_overlay_set *)b)->index;
    return x < y ? -1 : x > y;
}

static lhg_overlay_set *append_overlay(lhg_ctx *ctx, lhg_overlay_set **list, size_t *count, size_t *capacity)
{
    *list = lhg_grow(ctx, *list, capacity, *count, sizeof **list);
    memset(&(*list)[*count], 0, sizeof **list);
    return &(*list)[(*count)++];
}

/* The legacy baseline: the 24 top-table sets per disc, addressed by loader index. */
static int extract_disc(lhg_arena *arena, const char *cue, const char *work, const lhg_json *metadata,
                         const lhg_json *identity,
                         lhg_image **main_out, lhg_overlay_set **overlays, size_t *overlay_count, size_t *overlay_capacity)
{
    lhg_ctx *ctx = arena->ctx;
    lhg_disc disc;
    lhg_disc_open(&disc, arena, cue);
    const lhg_json *discs = lhg_json_require(ctx, metadata, "discs", "overlays.json");
    if (discs->type != LHG_JSON_OBJECT)
        lhg_fail(ctx, "overlays.json: discs must be an object");
    const lhg_json *info = NULL;
    int number = 0;
    size_t matches = 0;
    for (size_t index = 0; index < discs->as.object.count; index++) {
        const lhg_json_member *member = &discs->as.object.members[index];
        const char *exe = lhg_json_str(ctx, lhg_json_require(ctx, member->value, "exe", "disc"), "exe");
        const char *image = lhg_json_str(ctx, lhg_json_require(ctx, member->value, "image", "disc"), "image");
        if (lookup_exact(&disc, exe) && lookup_exact(&disc, image)) {
            info = member->value;
            if (member->key_length != 1 || member->key[0] < '1' || member->key[0] > '4')
                lhg_fail(ctx, "overlays.json: disc numbers must be 1 to 4");
            number = member->key[0] - '0';
            matches++;
        }
    }
    if (matches != 1)
        lhg_fail(ctx, "disc does not identify one supported FF8 USA disc");
    const lhg_json *rows = lhg_json_list(ctx, lhg_json_require(ctx, info, "overlays", "disc"), "overlays");
    size_t row_count = rows->as.array.count;
    const lhg_json **expected = lhg_arena_alloc(arena, (row_count + 1) * sizeof *expected);
    size_t expected_count = 0;
    for (size_t index = 0; index < row_count; index++) {
        const lhg_json *row = rows->as.array.items[index];
        int64_t value = lhg_json_int(ctx, lhg_json_require(ctx, row, "index", "overlay"), "overlay index");
        size_t slot = 0;
        while (slot < expected_count &&
               lhg_json_int(ctx, lhg_json_require(ctx, expected[slot], "index", "overlay"), "overlay index") != value)
            slot++;
        if (slot == expected_count)
            expected_count++;
        expected[slot] = row;
    }
    const char *destination = lhg_fmt(arena, "%s/overlays/disc%d", work, number);
    lhg_make_directories(ctx, destination);
    const char *exe_name = lhg_json_str(ctx, lhg_json_require(ctx, info, "exe", "disc"), "exe");
    const lhg_disc_file *record = lookup_exact(&disc, exe_name);
    uint8_t *exe = lhg_disc_read(&disc, record->record.lba, record->record.size);
    char sha1[41];
    lhg_sha1_hex(exe, record->record.size, sha1);
    if (identity && (record->record.size != lhg_json_int(ctx, lhg_json_require(ctx, identity, "file_size", "EXE identity"), "file_size") ||
        strcmp(sha1, lhg_json_str(ctx, lhg_json_require(ctx, identity, "sha1", "EXE identity"), "sha1"))))
        lhg_fail(ctx, "unsupported disc EXE identity");
    if (strcmp(sha1, lhg_json_str(ctx, lhg_json_require(ctx, metadata, "exe_sha1", "overlays.json"), "exe_sha1")))
        lhg_fail(ctx, "unsupported disc EXE SHA-1");
    lhg_write_if_changed(ctx, lhg_fmt(arena, "%s/SLUS_008.92", work), exe, record->record.size);
    lhg_image *image = parse_exe(arena, exe, record->record.size);
    apply_exe_metadata(arena, image, metadata, exe, record->record.size);

    const char *image_name = lhg_json_str(ctx, lhg_json_require(ctx, info, "image", "disc"), "image");
    const lhg_disc_file *image_record = lookup_exact(&disc, image_name);
    uint8_t *table = lhg_disc_read(&disc, image_record->record.lba, 2048);
    lhg_overlay_set *sets = NULL;
    size_t set_count = 0, set_capacity = 0;
    for (size_t position = 0; position < expected_count; position++) {
        const lhg_json *row = expected[position];
        int64_t raw_index = lhg_json_int(ctx, lhg_json_require(ctx, row, "index", "overlay"), "overlay index");
        if (raw_index < 0 || raw_index > 0xFFFF)
            lhg_fail(ctx, "overlay index out of range");
        int index = (int)raw_index;
        int64_t lba, size;
        const lhg_json *sector = lhg_json_find(row, "img_sector");
        if (sector) {
            int64_t offset = lhg_json_int(ctx, sector, "img_sector");
            if (offset < -(int64_t)UINT32_MAX || offset > (int64_t)UINT32_MAX)
                lhg_fail(ctx, "overlay %d is outside the disc", index);
            lba = (int64_t)image_record->record.lba + offset;
            size = lhg_json_int(ctx, lhg_json_require(ctx, row, "stored_size", "overlay"), "stored_size");
        } else {
            if (index < 0 || index >= 256)
                lhg_fail(ctx, "overlay %d has no top-table slot", index);
            lba = read_u32(table + (size_t)index * 8);
            size = read_u32(table + (size_t)index * 8 + 4);
        }
        if (lba < 0 || lba > (int64_t)UINT32_MAX)
            lhg_fail(ctx, "overlay %d is outside the disc", index);
        int compressed = lhg_json_truth(lhg_json_require(ctx, row, "compressed", "overlay"));
        uint8_t *raw = lhg_disc_read(&disc, (uint32_t)lba, size);
        size_t data_size = (size_t)size;
        uint8_t *data = compressed ? lhg_lzs_decompress(ctx, raw, (size_t)size, &data_size) : raw;
        const char *load_address = lhg_json_str(ctx, lhg_json_require(ctx, row, "load_address", "overlay"), "load_address");
        uint32_t base = parse_hex32(ctx, load_address, "load_address");
        char data_sha1[41];
        lhg_sha1_hex(data, data_size, data_sha1);
        if (strcmp(load_address, lhg_fmt(arena, "0x%X", base)) ||
            lhg_json_int(ctx, lhg_json_require(ctx, row, "size", "overlay"), "size") != (int64_t)data_size ||
            strcmp(lhg_json_str(ctx, lhg_json_require(ctx, row, "sha1", "overlay"), "sha1"), data_sha1))
            lhg_fail(ctx, "overlay %d does not match versioned metadata", index);
        if (compressed)
            lhg_free(ctx, raw);
        char digits[16];
        snprintf(digits, sizeof digits, "%02d", index);
        lhg_write_if_changed(ctx, lhg_fmt(arena, "%s/%s.bin", destination, digits), data, data_size);
        lhg_image *overlay = lhg_arena_alloc(arena, sizeof *overlay);
        memset(overlay, 0, sizeof *overlay);
        overlay->data = data;
        overlay->size = data_size;
        overlay->base = base;
        overlay->entry = entry_for_top(index, base);
        lhg_overlay_set *set = append_overlay(ctx, &sets, &set_count, &set_capacity);
        memset(set, 0, sizeof *set);
        set->index = (number - 1) * 32 + index;
        set->image = overlay;
    }
    lhg_free(ctx, table);
    lhg_disc_close(&disc);

    for (size_t index = 0; index < set_count; index++)
        for (size_t other = 0; other < *overlay_count; other++)
            if ((*overlays)[other].index == sets[index].index)
                lhg_fail(ctx, "the same disc was supplied more than once");
    if (*overlay_count && (image->size != (*main_out)->size || memcmp(image->data, (*main_out)->data, image->size)))
        lhg_fail(ctx, "disc EXEs differ");
    *main_out = image;
    for (size_t index = 0; index < set_count; index++)
        *append_overlay(ctx, overlays, overlay_count, overlay_capacity) = sets[index];
    lhg_free(ctx, sets);
    return number;
}

void lhg_load_legacy_discs(lhg_arena *arena, const char *catalog_dir, const char *const *cues, int cue_count,
                           const char *assets_dir, lhg_program *program, int verify_exe_identity)
{
    lhg_ctx *ctx = arena->ctx;
    const lhg_json *metadata = load_json(arena, catalog_dir, "overlays.json");
    const lhg_json *identity = verify_exe_identity ? load_json(arena, catalog_dir, "exe-identity.json") : NULL;
    lhg_make_directories(ctx, assets_dir);
    lhg_image *image = NULL;
    lhg_overlay_set *overlays = NULL;
    size_t overlay_count = 0, overlay_capacity = 0;
    unsigned disc_mask = 0;
    for (int index = 0; index < cue_count; index++) {
        int number = extract_disc(arena, cues[index], assets_dir, metadata, identity, &image, &overlays, &overlay_count, &overlay_capacity);
        if (disc_mask & (1u << number))
            lhg_fail(ctx, "the same disc was supplied more than once");
        disc_mask |= 1u << number;
        lhg_progress(ctx, "discs", (unsigned long)(index + 1), (unsigned long)cue_count);
    }
    if (overlay_count)
        qsort(overlays, overlay_count, sizeof *overlays, compare_set);
    memset(program, 0, sizeof *program);
    program->main = image;
    program->overlays = overlays;
    program->overlay_count = overlay_count;
    program->disc_mask = disc_mask;
}

void lhg_discover_legacy(lhg_arena *arena, lhg_program *program)
{
    for (size_t index = 0; index < program->overlay_count; index++) {
        lhg_overlay_set *set = &program->overlays[index];
        const lhg_image *image = set->image;
        size_t count;
        uint32_t *seeds = lhg_prologue_seeds(arena, image->data, image->size, (uint32_t)image->base, &count);
        lhg_discover(arena, image, seeds, count, 0, &set->functions);
    }
}

/* ---- full catalog ---- */

/* A hit inside one group means identical stored bytes: the decode is pure, so
 * the catalog's global cache adds no effect beyond speed once groups are done. */
typedef struct {
    const char *sha1_stored;
    int64_t read_size;
    uint8_t *data;
    size_t data_size;
    uint8_t *loaded;
    size_t loaded_size;
    int data_retained, loaded_retained;
} decode_entry;

typedef struct {
    const char *loaded_sha1;
    const lhg_json *location;
    uint8_t *loaded;
    size_t loaded_size;
    uint8_t *data;
    size_t data_size;
    decode_entry *owner;
} verified_entry;

/* Effect images were configured without a load address on some paths; the
 * absolute local jumps in their executable prefix anchor the true image base.
 * Assets may contain instruction-shaped words, so only the first 4 KiB, a
 * nearby target and an independently decodable nonzero target count as evidence.
 * Ties are rejected rather than guessing a missing configuration address. */
uint32_t lhg_infer_effect_base(lhg_arena *arena, const uint8_t *data, size_t size, const char *group_name,
                               lhg_json *evidence)
{
    lhg_ctx *ctx = arena->ctx;
    static const uint32_t candidates[3] = {0x80190000, 0x801A0000, 0x801B0000};
    lhg_json *examples[3];
    int counts[3] = {0, 0, 0};
    for (int candidate = 0; candidate < 3; candidate++) {
        uint32_t base = candidates[candidate];
        examples[candidate] = lhg_json_new_array(arena);
        lhg_image image;
        memset(&image, 0, sizeof image);
        image.data = data;
        image.size = size;
        image.base = base;
        image.entry = base;
        size_t limit = size < 4096 ? size : 4096;
        for (size_t offset = 0; offset + 3 < limit; offset += 4) {
            uint32_t word = read_u32(data + offset);
            if (word >> 26 != 2 && word >> 26 != 3)
                continue;
            int64_t target = (int64_t)(0x80000000u | ((word & 0x3FFFFFFu) << 2));
            int64_t destination = target - (int64_t)base;
            if (destination < 0 || destination > (int64_t)size - 4)
                continue;
            lhg_insn instruction = lhg_decode(read_u32(data + (size_t)destination), (uint32_t)target);
            int trampoline = offset == 0 && instruction.word >> 16 == 0x27BD && lhg_insn_signed_imm(&instruction) < 0;
            int64_t distance = destination - (int64_t)offset;
            if (distance < 0)
                distance = -distance;
            if (distance >= 0x4000 && !trampoline)
                continue;
            if (instruction.word && instruction.op != LHG_OP_RESERVED && instruction.op != LHG_OP_COP_UNUSABLE) {
                counts[candidate]++;
                if (counts[candidate] <= 8) {
                    lhg_json *row = lhg_json_new_object(arena);
                    lhg_json_set(arena, row, "offset", lhg_json_new_str(arena, lhg_fmt(arena, "%H", (uint64_t)offset)));
                    lhg_json_set(arena, row, "target", lhg_json_new_str(arena, lhg_fmt(arena, "%H", (uint64_t)target)));
                    lhg_json_set(arena, row, "target_offset", lhg_json_new_str(arena, lhg_fmt(arena, "%H", (uint64_t)destination)));
                    lhg_json_append(arena, examples[candidate], row);
                }
            }
        }
    }
    int order[3] = {0, 1, 2};
    /* A stable descending sort, so an earlier candidate wins equal counts for position only. */
    for (int index = 1; index < 3; index++) {
        int current = order[index], slot = index;
        while (slot && counts[order[slot - 1]] < counts[current]) {
            order[slot] = order[slot - 1];
            slot--;
        }
        order[slot] = current;
    }
    if (!counts[order[0]] || counts[order[0]] == counts[order[1]])
        lhg_fail(ctx, "%s: effect load address is ambiguous", group_name);
    uint32_t base = candidates[order[0]];
    lhg_json *votes = lhg_json_new_object(arena);
    for (int candidate = 0; candidate < 3; candidate++)
        lhg_json_set(arena, votes, lhg_fmt(arena, "%H", (uint64_t)candidates[candidate]), lhg_json_new_int(arena, counts[candidate]));
    lhg_json_set(arena, evidence, "name", lhg_json_new_str(arena, group_name));
    lhg_json_set(arena, evidence, "method", lhg_json_new_str(arena, "absolute local j/jal in first 4 KiB; nearby targets or an entry trampoline to a prologue; reject ties"));
    lhg_json_set(arena, evidence, "base", lhg_json_new_str(arena, lhg_fmt(arena, "%H", (uint64_t)base)));
    lhg_json_set(arena, evidence, "votes", votes);
    lhg_json_set(arena, evidence, "examples", examples[order[0]]);
    return base;
}

/* Small open-addressed index from inventory path to its first entry. */
typedef struct {
    const char *key;
    const lhg_json *value;
} path_slot;

typedef struct {
    path_slot *slots;
    size_t capacity;
} path_index;

static uint64_t hash_text(const char *text)
{
    return lhg_fnv64_add(LHG_FNV64_INIT, (const uint8_t *)text, strlen(text));
}

static void path_index_build(lhg_arena *arena, path_index *index, const lhg_json *entries)
{
    lhg_ctx *ctx = arena->ctx;
    size_t capacity = 16;
    while (capacity < entries->as.array.count * 2 + 16)
        capacity *= 2;
    index->slots = lhg_arena_alloc(arena, capacity * sizeof *index->slots);
    memset(index->slots, 0, capacity * sizeof *index->slots);
    index->capacity = capacity;
    for (size_t row = 0; row < entries->as.array.count; row++) {
        const lhg_json *entry = entries->as.array.items[row];
        const char *path = lhg_json_str(ctx, lhg_json_require(ctx, entry, "path", "inventory entry"), "path");
        size_t slot = (size_t)hash_text(path) & (capacity - 1);
        while (index->slots[slot].key && strcmp(index->slots[slot].key, path))
            slot = (slot + 1) & (capacity - 1);
        /* The first entry of a path wins, as table order defines the loader's view. */
        if (!index->slots[slot].key) {
            index->slots[slot].key = path;
            index->slots[slot].value = entry;
        }
    }
}

static const lhg_json *path_index_find(const path_index *index, const char *path)
{
    size_t slot = (size_t)hash_text(path) & (index->capacity - 1);
    while (index->slots[slot].key) {
        if (!strcmp(index->slots[slot].key, path))
            return index->slots[slot].value;
        slot = (slot + 1) & (index->capacity - 1);
    }
    return NULL;
}

/* Per-set entry hints sit beside the executable's in overlays.json, keyed by a
 * set's catalog SHA-1: overlays-all.json is imported, while these are found by
 * replaying the game, and name functions reached only through pointers. Every
 * list is sorted and unique, so a set's hints have one spelling and one identity. */
static void check_set_hints(lhg_ctx *ctx, const lhg_json *metadata, const lhg_json *catalog)
{
    const lhg_json *rows = lhg_json_find(metadata, "set_entry_points");
    if (!rows)
        return;
    lhg_json_list(ctx, rows, "set_entry_points");
    for (size_t row = 0; row < rows->as.array.count; row++) {
        const lhg_json *item = rows->as.array.items[row];
        const char *name = lhg_json_str(ctx, lhg_json_require(ctx, item, "name", "set_entry_points"), "name");
        const char *sha1 = lhg_json_str(ctx, lhg_json_require(ctx, item, "sha1", "set_entry_points"), "sha1");
        const lhg_json *points = lhg_json_list(ctx, lhg_json_require(ctx, item, "entry_points", name), "entry_points");
        if (!points->as.array.count)
            lhg_fail(ctx, "set_entry_points: %s %s lists no entry points", name, sha1);
        uint32_t previous = 0;
        for (size_t at = 0; at < points->as.array.count; at++) {
            const char *text = lhg_json_str(ctx, points->as.array.items[at], "entry_points");
            uint32_t pc = parse_hex32(ctx, text, "entry_points");
            char spelled[16];
            snprintf(spelled, sizeof spelled, "0x%08lx", (unsigned long)pc);
            if (strcmp(text, spelled))
                lhg_fail(ctx, "set_entry_points: %s is not spelled 0x and eight lowercase digits", text);
            if (pc & 3 || (at && pc <= previous))
                lhg_fail(ctx, "set_entry_points: %s %s must list aligned addresses in ascending order", name, sha1);
            previous = pc;
        }
        for (size_t other = 0; other < row; other++)
            if (!strcmp(sha1, lhg_json_str(ctx, lhg_json_require(ctx, rows->as.array.items[other], "sha1", "set_entry_points"), "sha1")))
                lhg_fail(ctx, "set_entry_points: %s is listed twice", sha1);
        size_t matches = 0;
        for (size_t group = 0; group < catalog->as.array.count; group++) {
            const lhg_json *candidate = catalog->as.array.items[group];
            if (strcmp(sha1, lhg_json_str(ctx, lhg_json_require(ctx, candidate, "sha1", "catalog group"), "sha1")))
                continue;
            if (strcmp(name, lhg_json_str(ctx, lhg_json_require(ctx, candidate, "name", "catalog group"), "name")))
                lhg_fail(ctx, "set_entry_points: %s is not named %s in the catalog", sha1, name);
            matches++;
        }
        if (matches != 1)
            lhg_fail(ctx, "set_entry_points: %s %s does not name one catalog set", name, sha1);
    }
}

/* The checked hint list of one catalog group, or NULL. */
static const lhg_json *set_hints(lhg_ctx *ctx, const lhg_json *metadata, const lhg_json *group, const char *group_name)
{
    const lhg_json *rows = lhg_json_find(metadata, "set_entry_points");
    if (!rows)
        return NULL;
    const char *sha1 = lhg_json_str(ctx, lhg_json_require(ctx, group, "sha1", group_name), "sha1");
    for (size_t row = 0; row < rows->as.array.count; row++)
        if (!strcmp(sha1, lhg_json_str(ctx, lhg_json_require(ctx, rows->as.array.items[row], "sha1", "set_entry_points"), "sha1")))
            return lhg_json_require(ctx, rows->as.array.items[row], "entry_points", group_name);
    return NULL;
}

/* Which sets the present discs give, and their loader indices, from metadata
 * alone. Within a group, images with the same loaded bytes are one set held by
 * the last location listing them; set numbers below 128 stay the original
 * top-table overlays, and the rest are numbered in catalog order. Returns the
 * next free number, or -1 when a set has no inventory entry. */
static int plan_groups(lhg_arena *arena, const lhg_json *metadata, const lhg_json *catalog, const int present[5],
                       const path_index indexes[5], int strict, lhg_plan_row **out, size_t *out_count)
{
    lhg_ctx *ctx = arena->ctx;
    lhg_plan_row *rows = NULL;
    size_t count = 0, capacity = 0;
    int extra = 128;
    for (size_t group_index = 0; group_index < catalog->as.array.count; group_index++) {
        const lhg_json *group = catalog->as.array.items[group_index];
        const char *group_name = lhg_json_str(ctx, lhg_json_require(ctx, group, "name", "catalog group"), "name");
        const lhg_json *locations = lhg_json_list(ctx, lhg_json_require(ctx, group, "locations", group_name), "locations");
        const lhg_json *hints = set_hints(ctx, metadata, group, group_name);
        size_t first = count;
        for (size_t row = 0; row < locations->as.array.count; row++) {
            const lhg_json *location = locations->as.array.items[row];
            int64_t disc = lhg_json_int(ctx, lhg_json_require(ctx, location, "disc", "location"), "disc");
            if (disc < 1 || disc > 4 || !present[disc])
                continue;
            const char *loaded = lhg_json_str(ctx, lhg_json_require(ctx, location, "loaded_sha1", "location"), "loaded_sha1");
            size_t slot = first;
            while (slot < count && strcmp(rows[slot].loaded_sha1, loaded))
                slot++;
            if (slot == count) {
                rows = lhg_grow(ctx, rows, &capacity, count, sizeof *rows);
                memset(&rows[count++], 0, sizeof *rows);
            }
            rows[slot].group = group_index;
            rows[slot].group_json = group;
            rows[slot].group_name = group_name;
            rows[slot].hints = hints;
            rows[slot].loaded_sha1 = loaded;
            rows[slot].location = location;
            rows[slot].disc = (int)disc;
            rows[slot].discs |= 1u << disc;
        }
        for (size_t slot = first; slot < count; slot++) {
            lhg_plan_row *item = &rows[slot];
            const char *path = lhg_json_str(ctx, lhg_json_require(ctx, item->location, "path", "location"), "path");
            const lhg_json *inventory_entry = path_index_find(&indexes[item->disc], path);
            if (!inventory_entry && strict)
                lhg_fail(ctx, "%s has no inventory entry on disc %d", path, item->disc);
            /* A loader reports it where it always did: after the bytes were checked. */
            if (!inventory_entry) {
                item->missing = 1;
                *out = rows;
                *out_count = slot + 1;
                return -1;
            }
            const lhg_json *locator = lhg_json_require(ctx, inventory_entry, "locator", "inventory entry");
            int has_top = !strcmp(lhg_json_str(ctx, lhg_json_require(ctx, locator, "table", "locator"), "table"), "top");
            int64_t raw_top = has_top ? lhg_json_int(ctx, lhg_json_require(ctx, locator, "index", "locator"), "index") : -1;
            /* Anything outside a top table's slots is simply not a known loader index. */
            int top = raw_top >= 0 && raw_top < 256 ? (int)raw_top : -1;
            int index = has_top && is_legacy_top(top) ? (item->disc - 1) * 32 + top : extra;
            /* Comparing the result rather than the path mirrors how indices were assigned. */
            if (index == extra)
                extra++;
            item->index = index;
            item->has_top = has_top;
            item->top = top;
        }
    }
    *out = rows;
    *out_count = count;
    return extra;
}

/* Code prefixes: assets whose first bytes are code, loaded at one address,
 * with data after it (battle stages). A record lists each distinct prefix as a
 * variant: its SHA-1, its length and the assets that start with it. Lengths
 * come from bounded discovery within the record's limit, so the generator and
 * ff8files verify can both recompute them from the disc. */
static int is_sha1_text(const char *text)
{
    size_t length = strlen(text);
    if (length != 40)
        return 0;
    for (size_t at = 0; at < length; at++)
        if (!((text[at] >= '0' && text[at] <= '9') || (text[at] >= 'a' && text[at] <= 'f')))
            return 0;
    return 1;
}

static void check_code_prefixes(lhg_ctx *ctx, const lhg_json *metadata)
{
    const lhg_json *records = lhg_json_find(metadata, "code_prefixes");
    if (!records)
        return;
    lhg_json_list(ctx, records, "code_prefixes");
    for (size_t at = 0; at < records->as.array.count; at++) {
        const lhg_json *record = records->as.array.items[at];
        const char *name = lhg_json_str(ctx, lhg_json_require(ctx, record, "name", "code_prefixes"), "name");
        if (!*name || strlen(name) >= sizeof ((lhg_set_info *)0)->name)
            lhg_fail(ctx, "code_prefixes: a record name must have 1 to 31 characters");
        const char *address = lhg_json_str(ctx, lhg_json_require(ctx, record, "load_address", name), "load_address");
        uint32_t base = parse_hex32(ctx, address, "load_address");
        char spelled[16];
        snprintf(spelled, sizeof spelled, "0x%08lx", (unsigned long)base);
        int64_t limit = lhg_json_int(ctx, lhg_json_require(ctx, record, "limit", name), "limit");
        if (strcmp(address, spelled) || base & 3 || limit <= 0 || limit % 4 || limit > 0x100000)
            lhg_fail(ctx, "code_prefixes: %s needs an aligned load address spelled 0x%%08x and a limit of 4 bytes to 1 MiB", name);
        const lhg_json *variants = lhg_json_list(ctx, lhg_json_require(ctx, record, "variants", name), "variants");
        if (!variants->as.array.count)
            lhg_fail(ctx, "code_prefixes: %s lists no variants", name);
        for (size_t row = 0; row < variants->as.array.count; row++) {
            const lhg_json *variant = variants->as.array.items[row];
            const char *sha1 = lhg_json_str(ctx, lhg_json_require(ctx, variant, "sha1", name), "sha1");
            int64_t size = lhg_json_int(ctx, lhg_json_require(ctx, variant, "size", name), "size");
            const lhg_json *paths = lhg_json_list(ctx, lhg_json_require(ctx, variant, "paths", name), "paths");
            if (!is_sha1_text(sha1) || size <= 0 || size % 4 || size > limit || !paths->as.array.count)
                lhg_fail(ctx, "code_prefixes: %s variant %lu needs a SHA-1, an aligned size within the limit and its paths", name,
                         (unsigned long)row);
            for (size_t path = 0; path < paths->as.array.count; path++) {
                const char *text = lhg_json_str(ctx, paths->as.array.items[path], "paths");
                /* An asset starts with one prefix, so it belongs to one variant of one record. */
                for (size_t other_record = 0; other_record <= at; other_record++) {
                    const lhg_json *others = lhg_json_require(ctx, records->as.array.items[other_record], "variants", "code_prefixes");
                    size_t rows = other_record == at ? row + 1 : others->as.array.count;
                    for (size_t other_row = 0; other_row < rows; other_row++) {
                        const lhg_json *other_paths = lhg_json_require(ctx, others->as.array.items[other_row], "paths", "code_prefixes");
                        size_t stop = other_record == at && other_row == row ? path : other_paths->as.array.count;
                        for (size_t other = 0; other < stop; other++)
                            if (!strcmp(text, lhg_json_str(ctx, other_paths->as.array.items[other], "paths")))
                                lhg_fail(ctx, "code_prefixes: %s is listed twice", text);
                    }
                }
            }
        }
    }
}

/* One row per record whose assets the present discs hold, after every catalog
 * group. The row takes the number of its first variant and the later variants
 * the numbers after it, so no two images share a set index. An asset may differ
 * between discs after its code; the code itself is checked on every disc. */
static void plan_prefixes(lhg_ctx *ctx, const lhg_json *metadata, const int present[5], const path_index indexes[5],
                          lhg_plan_row **rows, size_t *count, size_t *capacity, int extra)
{
    const lhg_json *records = lhg_json_find(metadata, "code_prefixes");
    if (!records)
        return;
    for (size_t at = 0; at < records->as.array.count; at++) {
        const lhg_json *record = records->as.array.items[at];
        const char *name = lhg_json_str(ctx, lhg_json_require(ctx, record, "name", "code_prefixes"), "name");
        const lhg_json *variants = lhg_json_require(ctx, record, "variants", name);
        unsigned discs = 0;
        int first = 0;
        for (int disc = 1; disc <= 4; disc++) {
            if (!present[disc])
                continue;
            size_t held = 0, total = 0;
            for (size_t row = 0; row < variants->as.array.count; row++) {
                const lhg_json *paths = lhg_json_require(ctx, variants->as.array.items[row], "paths", name);
                for (size_t path = 0; path < paths->as.array.count; path++, total++) {
                    const char *text = lhg_json_str(ctx, paths->as.array.items[path], "paths");
                    if (path_index_find(&indexes[disc], text))
                        held++;
                }
            }
            if (held && held != total)
                lhg_fail(ctx, "%s: disc %d holds only some of its assets", name, disc);
            if (held) {
                discs |= 1u << disc;
                if (!first)
                    first = disc;
            }
        }
        if (!discs)
            continue;
        *rows = lhg_grow(ctx, *rows, capacity, *count, sizeof **rows);
        lhg_plan_row *item = &(*rows)[(*count)++];
        memset(item, 0, sizeof *item);
        item->group_name = name;
        item->prefix = record;
        item->index = extra;
        item->disc = first;
        item->discs = discs;
        item->top = -1;
        extra += (int)variants->as.array.count;
    }
}

/* Verifies every asset of a prefix row on every present disc and discovers
 * each variant once, as consecutive sets that share the row's overlay row. */
static void compile_prefix(lhg_arena *arena, const lhg_plan_row *row, lhg_ff8_image discs[5], const path_index indexes[5],
                           const char *assets_dir, lhg_overlay_set **result, size_t *count, size_t *capacity)
{
    lhg_ctx *ctx = arena->ctx;
    const lhg_json *record = row->prefix;
    const char *name = row->group_name;
    uint32_t base = parse_hex32(ctx, lhg_json_str(ctx, lhg_json_require(ctx, record, "load_address", name), "load_address"), "load_address");
    size_t limit = (size_t)lhg_json_int(ctx, lhg_json_require(ctx, record, "limit", name), "limit");
    const lhg_json *variants = lhg_json_require(ctx, record, "variants", name);
    lhg_json *names = lhg_json_new_array(arena);
    lhg_json_append(arena, names, lhg_json_new_str(arena, name));
    for (size_t row_index = 0; row_index < variants->as.array.count; row_index++) {
        const lhg_json *variant = variants->as.array.items[row_index];
        const char *sha1 = lhg_json_str(ctx, lhg_json_require(ctx, variant, "sha1", name), "sha1");
        int64_t size = lhg_json_int(ctx, lhg_json_require(ctx, variant, "size", name), "size");
        const lhg_json *paths = lhg_json_require(ctx, variant, "paths", name);
        lhg_image *image = NULL;
        lhg_functions functions;
        memset(&functions, 0, sizeof functions);
        for (size_t at = 0; at < paths->as.array.count * 4; at++) {
            size_t path = at / 4;
            int number = (int)(at % 4) + 1;
            if (!(row->discs & (1u << number)))
                continue;
            lhg_ff8_image *disc = &discs[number];
            const char *text = lhg_json_str(ctx, paths->as.array.items[path], "paths");
            const lhg_json *entry = path_index_find(&indexes[number], text);
            int64_t sector = lhg_json_int(ctx, lhg_json_require(ctx, entry, "sector", text), "sector");
            int64_t stored = lhg_json_int(ctx, lhg_json_require(ctx, entry, "size", text), "size");
            uint8_t *raw = lhg_ff8_image_read(disc, sector, stored);
            char digest[41];
            lhg_sha1_hex(raw, (size_t)stored, digest);
            if (strcmp(digest, lhg_json_str(ctx, lhg_json_require(ctx, entry, "sha1", text), "sha1")))
                lhg_fail(ctx, "catalog bytes disagree: %s disc %d", text, number);
            size_t decoded_size;
            uint8_t *decoded = lhg_lzs_decompress(ctx, raw, (size_t)stored, &decoded_size);
            lhg_free(ctx, raw);
            lhg_image candidate;
            memset(&candidate, 0, sizeof candidate);
            candidate.data = decoded;
            candidate.size = decoded_size;
            candidate.base = base;
            candidate.entry = base;
            lhg_functions found;
            size_t length = lhg_code_prefix(arena, &candidate, limit, &found);
            lhg_sha1_hex(decoded, length, digest);
            if ((int64_t)length != size || strcmp(digest, sha1))
                lhg_fail(ctx, "%s: %s on disc %d starts with %lu bytes of code with SHA-1 %s, not the catalog's %lld bytes with %s", name,
                         text, number, (unsigned long)length, digest, (long long)size, sha1);
            if (!image) {
                uint8_t *bytes = lhg_arena_alloc(arena, length);
                memcpy(bytes, decoded, length);
                image = lhg_arena_alloc(arena, sizeof *image);
                *image = candidate;
                image->data = bytes;
                functions = found;
            }
            lhg_free(ctx, decoded);
        }
        lhg_image_meta *meta = lhg_arena_alloc(arena, sizeof *meta);
        memset(meta, 0, sizeof *meta);
        meta->name = name;
        meta->names = names;
        meta->paths = paths;
        for (int number = 1; number <= 4; number++)
            if (row->discs & (1u << number))
                meta->discs[meta->disc_count++] = number;
        meta->catalog_sha1 = sha1;
        meta->catalog_size = size;
        meta->has_disc = meta->disc_count == 1;
        meta->disc = row->disc;
        image->meta = meta;
        lhg_overlay_set *set = append_overlay(ctx, result, count, capacity);
        memset(set, 0, sizeof *set);
        set->index = row->index + (int)row_index;
        set->image = image;
        set->functions = functions;
        set->shares_row = row_index > 0;
        if (assets_dir)
            lhg_write_file(ctx, lhg_fmt(arena, "%s/set%d.bin", assets_dir, set->index), image->data, image->size);
    }
}

/* What a prefix row's code comes from: the load address, the discovery bound
 * and each variant's bytes and length, in the order that numbers the variants. */
static void prefix_identity(lhg_ctx *ctx, const lhg_plan_row *row, char hex[65])
{
    lhg_sha256 hash;
    char line[160];
    lhg_sha256_init(&hash);
    lhg_sha256_add(&hash, "lionheart-prefix-1\n", 19);
    snprintf(line, sizeof line, "%s\n%s\n%lld\n", row->group_name,
             lhg_json_str(ctx, lhg_json_require(ctx, row->prefix, "load_address", row->group_name), "load_address"),
             (long long)lhg_json_int(ctx, lhg_json_require(ctx, row->prefix, "limit", row->group_name), "limit"));
    lhg_sha256_add(&hash, line, strlen(line));
    const lhg_json *variants = lhg_json_require(ctx, row->prefix, "variants", row->group_name);
    for (size_t at = 0; at < variants->as.array.count; at++) {
        const lhg_json *variant = variants->as.array.items[at];
        snprintf(line, sizeof line, "%s %lld\n", lhg_json_str(ctx, lhg_json_require(ctx, variant, "sha1", row->group_name), "sha1"),
                 (long long)lhg_json_int(ctx, lhg_json_require(ctx, variant, "size", row->group_name), "size"));
        lhg_sha256_add(&hash, line, strlen(line));
    }
    lhg_sha256_finish(&hash, hex);
}

void lhg_plan_identity(lhg_ctx *ctx, const lhg_plan_row *row, char hex[65])
{
    if (row->prefix) {
        prefix_identity(ctx, row, hex);
        return;
    }
    const lhg_json *load_address = lhg_json_require(ctx, row->location, "load_address", "location");
    lhg_sha256 hash;
    char number[64];
    lhg_sha256_init(&hash);
    lhg_sha256_add(&hash, "lionheart-set-1\n", 16);
    const char *parts[3] = {lhg_json_str(ctx, lhg_json_require(ctx, row->group_json, "sha1", row->group_name), "sha1"), row->loaded_sha1,
                            lhg_json_truth(load_address) ? lhg_json_str(ctx, load_address, "load_address") : "inferred"};
    for (int part = 0; part < 3; part++) {
        lhg_sha256_add(&hash, parts[part], strlen(parts[part]));
        lhg_sha256_add(&hash, "\n", 1);
    }
    snprintf(number, sizeof number, "%lld %d %d\n",
             (long long)lhg_json_int(ctx, lhg_json_require(ctx, row->location, "loaded_size", "location"), "loaded_size"), row->has_top,
             row->top);
    lhg_sha256_add(&hash, number, strlen(number));
    /* Only a hinted set hashes its hints, so every other identity stays as it was. */
    if (row->hints)
        for (size_t at = 0; at < row->hints->as.array.count; at++) {
            const char *pc = lhg_json_str(ctx, row->hints->as.array.items[at], "entry_points");
            lhg_sha256_add(&hash, "entry ", 6);
            lhg_sha256_add(&hash, pc, strlen(pc));
            lhg_sha256_add(&hash, "\n", 1);
        }
    lhg_sha256_finish(&hash, hex);
}

uint32_t lhg_plan_base(lhg_ctx *ctx, const lhg_plan_row *row)
{
    if (row->prefix)
        return parse_hex32(ctx, lhg_json_str(ctx, lhg_json_require(ctx, row->prefix, "load_address", row->group_name), "load_address"),
                           "load_address");
    const lhg_json *load_address = lhg_json_require(ctx, row->location, "load_address", "location");
    return lhg_json_truth(load_address) ? parse_hex32(ctx, lhg_json_str(ctx, load_address, "load_address"), "load_address") : 0;
}

uint32_t lhg_plan_size(lhg_ctx *ctx, const lhg_plan_row *row)
{
    if (!row->prefix)
        return (uint32_t)lhg_json_int(ctx, lhg_json_require(ctx, row->location, "loaded_size", "location"), "loaded_size");
    const lhg_json *variants = lhg_json_require(ctx, row->prefix, "variants", row->group_name);
    int64_t longest = 0;
    for (size_t at = 0; at < variants->as.array.count; at++) {
        int64_t size = lhg_json_int(ctx, lhg_json_require(ctx, variants->as.array.items[at], "size", row->group_name), "size");
        if (size > longest)
            longest = size;
    }
    return (uint32_t)longest;
}

void lhg_plan_catalog(lhg_arena *arena, const char *catalog_dir, unsigned disc_mask, lhg_plan_row **rows, size_t *count)
{
    lhg_ctx *ctx = arena->ctx;
    const lhg_json *metadata = load_json(arena, catalog_dir, "overlays.json");
    const lhg_json *catalog_root = load_json(arena, catalog_dir, "overlays-all.json");
    const lhg_json *catalog = lhg_json_list(ctx, lhg_json_require(ctx, catalog_root, "overlays", "overlays-all.json"), "overlays");
    int present[5] = {0};
    path_index indexes[5];
    memset(indexes, 0, sizeof indexes);
    for (int number = 1; number <= 4; number++) {
        if (!(disc_mask & (1u << number)))
            continue;
        const lhg_json *inventory = load_json(arena, catalog_dir, lhg_fmt(arena, "files-disc%d.json", number));
        path_index_build(arena, &indexes[number], lhg_json_list(ctx, lhg_json_require(ctx, inventory, "entries", "inventory"), "entries"));
        present[number] = 1;
    }
    check_set_hints(ctx, metadata, catalog);
    check_code_prefixes(ctx, metadata);
    int extra = plan_groups(arena, metadata, catalog, present, indexes, 1, rows, count);
    size_t capacity = *count;
    plan_prefixes(ctx, metadata, present, indexes, rows, count, &capacity, extra);
}

void lhg_compile_catalog(lhg_arena *arena, const char *catalog_dir, const char *const *cues, int cue_count,
                         const char *assets_dir, lhg_program *program)
{
    lhg_ctx *ctx = arena->ctx;
    /* A host emitting modules itself keeps every image in memory. */
    if (assets_dir)
        lhg_make_directories(ctx, assets_dir);
    const lhg_json *metadata = load_json(arena, catalog_dir, "overlays.json");
    const lhg_json *catalog_root = load_json(arena, catalog_dir, "overlays-all.json");
    const lhg_json *catalog = lhg_json_list(ctx, lhg_json_require(ctx, catalog_root, "overlays", "overlays-all.json"), "overlays");

    lhg_ff8_image discs[5];
    int present[5] = {0};
    path_index indexes[5];
    lhg_image *main_image = NULL;
    const uint8_t *previous_exe = NULL;
    size_t previous_size = 0;
    for (int cue = 0; cue < cue_count; cue++) {
        lhg_ff8_image image;
        lhg_ff8_image_open(&image, arena, cues[cue]);
        if (present[image.number])
            lhg_fail(ctx, "the same disc was supplied more than once");
        discs[image.number] = image;
        present[image.number] = 1;
        const lhg_json *inventory = load_json(arena, catalog_dir, lhg_fmt(arena, "files-disc%d.json", image.number));
        const lhg_json *layout = lhg_json_require(ctx, inventory, "image", "inventory");
        if (lhg_json_int(ctx, lhg_json_require(ctx, layout, "lba", "inventory image"), "lba") != (int64_t)image.record.lba ||
            lhg_json_int(ctx, lhg_json_require(ctx, layout, "size", "inventory image"), "size") != (int64_t)image.record.size)
            lhg_fail(ctx, "disc IMG does not match inventory layout");
        path_index_build(arena, &indexes[image.number], lhg_json_list(ctx, lhg_json_require(ctx, inventory, "entries", "inventory"), "entries"));
        const char *exe_name = lhg_json_str(ctx, lhg_json_require(ctx, lhg_json_require(ctx, inventory, "exe", "inventory"), "name", "inventory exe"), "exe name");
        const lhg_disc_file *row = lhg_disc_find(&discs[image.number].disc, exe_name);
        size_t exe_size = row->record.size;
        uint8_t *exe = lhg_disc_read(&discs[image.number].disc, row->record.lba, (int64_t)exe_size);
        if (previous_exe && (previous_size != exe_size || memcmp(previous_exe, exe, exe_size)))
            lhg_fail(ctx, "disc EXEs differ");
        if (assets_dir)
            lhg_write_file(ctx, lhg_fmt(arena, "%s/SLUS_008.92", assets_dir), exe, exe_size);
        main_image = parse_exe(arena, exe, exe_size);
        apply_exe_metadata(arena, main_image, metadata, exe, exe_size);
        previous_exe = exe;
        previous_size = exe_size;
        lhg_progress(ctx, "discs", (unsigned long)(cue + 1), (unsigned long)cue_count);
    }

    lhg_plan_row *plan;
    size_t plan_count, plan_next = 0;
    check_set_hints(ctx, metadata, catalog);
    check_code_prefixes(ctx, metadata);
    int extra = plan_groups(arena, metadata, catalog, present, indexes, 0, &plan, &plan_count);
    if (extra >= 0) {
        size_t capacity = plan_count;
        plan_prefixes(ctx, metadata, present, indexes, &plan, &plan_count, &capacity, extra);
    }
    lhg_json *evidence = lhg_json_new_object(arena);
    lhg_overlay_set *result = NULL;
    size_t result_count = 0, result_capacity = 0;
    for (size_t group_index = 0; group_index < catalog->as.array.count; group_index++) {
        const lhg_json *group = catalog->as.array.items[group_index];
        const char *group_name = lhg_json_str(ctx, lhg_json_require(ctx, group, "name", "catalog group"), "name");
        const lhg_json *locations = lhg_json_list(ctx, lhg_json_require(ctx, group, "locations", group_name), "locations");
        size_t location_count = locations->as.array.count;
        const lhg_json **chosen = lhg_alloc(ctx, (location_count + 1) * sizeof *chosen);
        size_t chosen_count = 0;
        for (size_t row = 0; row < location_count; row++) {
            const lhg_json *location = locations->as.array.items[row];
            int64_t disc = lhg_json_int(ctx, lhg_json_require(ctx, location, "disc", "location"), "disc");
            if (disc >= 1 && disc <= 4 && present[disc])
                chosen[chosen_count++] = location;
        }
        if (!chosen_count) {
            lhg_free(ctx, chosen);
            continue;
        }

        const char *group_sha1 = lhg_json_str(ctx, lhg_json_require(ctx, group, "sha1", group_name), "sha1");
        int64_t group_size = lhg_json_int(ctx, lhg_json_require(ctx, group, "size", group_name), "size");
        decode_entry *decoded = lhg_alloc(ctx, chosen_count * sizeof *decoded);
        size_t decoded_count = 0;
        verified_entry *verified = lhg_alloc(ctx, chosen_count * sizeof *verified);
        size_t verified_count = 0;
        int present_mask = 0;
        for (size_t row = 0; row < chosen_count; row++) {
            const lhg_json *location = chosen[row];
            int disc = (int)lhg_json_int(ctx, lhg_json_require(ctx, location, "disc", "location"), "disc");
            present_mask |= 1 << disc;
            int64_t sector = lhg_json_int(ctx, lhg_json_require(ctx, location, "sector", "location"), "sector");
            int64_t size = lhg_json_int(ctx, lhg_json_require(ctx, location, "size", "location"), "size");
            uint8_t *raw = lhg_ff8_image_read(&discs[disc], sector, size);
            const char *sha1_stored = lhg_json_str(ctx, lhg_json_require(ctx, location, "sha1_stored", "location"), "sha1_stored");
            int64_t read_size = lhg_json_int(ctx, lhg_json_require(ctx, location, "read_size", "location"), "read_size");
            int compressed = lhg_json_truth(lhg_json_require(ctx, location, "compressed", "location"));
            if (read_size < 0)
                lhg_fail(ctx, "catalog read_size is negative");
            decode_entry *entry = NULL;
            for (size_t slot = 0; slot < decoded_count; slot++)
                if (decoded[slot].read_size == read_size && !strcmp(decoded[slot].sha1_stored, sha1_stored))
                    entry = &decoded[slot];
            if (!entry) {
                entry = &decoded[decoded_count++];
                memset(entry, 0, sizeof *entry);
                entry->sha1_stored = sha1_stored;
                entry->read_size = read_size;
                if (compressed) {
                    entry->data = lhg_lzs_decompress(ctx, raw, (size_t)size, &entry->data_size);
                    if (read_size < size)
                        entry->loaded = lhg_lzs_decompress(ctx, raw, (size_t)read_size, &entry->loaded_size);
                    else {
                        entry->loaded = entry->data;
                        entry->loaded_size = entry->data_size;
                    }
                } else {
                    entry->data = raw;
                    entry->data_size = (size_t)size;
                    entry->loaded = raw;
                    entry->loaded_size = (size_t)size;
                }
            }
            char sha1[41];
            lhg_sha1_hex(raw, (size_t)size, sha1);
            const char *path = lhg_json_str(ctx, lhg_json_require(ctx, location, "path", "location"), "path");
            char data_sha1[41];
            lhg_sha1_hex(entry->data, entry->data_size, data_sha1);
            if (strcmp(sha1, sha1_stored) || (int64_t)entry->data_size != group_size || strcmp(data_sha1, group_sha1))
                lhg_fail(ctx, "catalog bytes disagree: %s disc %d", path, disc);
            char loaded_sha1[41];
            lhg_sha1_hex(entry->loaded, entry->loaded_size, loaded_sha1);
            const char *expected_loaded = lhg_json_str(ctx, lhg_json_require(ctx, location, "loaded_sha1", "location"), "loaded_sha1");
            if ((int64_t)entry->loaded_size != lhg_json_int(ctx, lhg_json_require(ctx, location, "loaded_size", "location"), "loaded_size") ||
                strcmp(loaded_sha1, expected_loaded))
                lhg_fail(ctx, "loader bytes disagree: %s", path);
            if (raw != entry->data)
                lhg_free(ctx, raw);
            size_t slot = 0;
            while (slot < verified_count && strcmp(verified[slot].loaded_sha1, expected_loaded))
                slot++;
            if (slot == verified_count)
                verified_count++;
            verified[slot].loaded_sha1 = expected_loaded;
            verified[slot].location = location;
            verified[slot].loaded = entry->loaded;
            verified[slot].loaded_size = entry->loaded_size;
            verified[slot].data = entry->data;
            verified[slot].data_size = entry->data_size;
            verified[slot].owner = entry;
        }
        int distinct = 0;
        for (int disc = 1; disc <= 4; disc++)
            if (present_mask & (1 << disc))
                distinct++;

        /* Loader-bounded tails differ on the compressed top-table images:
         * emit those actual RAM bytes while keeping the complete catalog identity. */
        for (size_t row = 0; row < verified_count; row++) {
            verified_entry *item = &verified[row];
            const lhg_json *location = item->location;
            int disc = (int)lhg_json_int(ctx, lhg_json_require(ctx, location, "disc", "location"), "disc");
            const lhg_json *load_address = lhg_json_require(ctx, location, "load_address", "location");
            uint32_t base;
            if (lhg_json_truth(load_address)) {
                base = parse_hex32(ctx, lhg_json_str(ctx, load_address, "load_address"), "load_address");
            } else {
                lhg_json *proof = lhg_json_new_object(arena);
                base = lhg_infer_effect_base(arena, item->data, item->data_size, group_name, proof);
                lhg_json_set(arena, evidence, group_sha1, proof);
            }
            /* The plan walked the same groups and locations in the same order. */
            const lhg_plan_row *planned = plan_next < plan_count ? &plan[plan_next++] : NULL;
            if (planned && planned->missing)
                lhg_fail(ctx, "%s has no inventory entry on disc %d",
                         lhg_json_str(ctx, lhg_json_require(ctx, location, "path", "location"), "path"), disc);
            if (!planned || planned->group != group_index || planned->location != location || strcmp(planned->loaded_sha1, item->loaded_sha1))
                lhg_fail(ctx, "%s: catalog plan disagrees with the verified images", group_name);
            int has_top = planned->has_top, top = planned->top, index = planned->index;
            lhg_image *image = lhg_arena_alloc(arena, sizeof *image);
            memset(image, 0, sizeof *image);
            image->data = item->loaded;
            image->size = item->loaded_size;
            image->base = base;
            image->entry = has_top ? entry_for_top(top, base) : base;
            lhg_image_meta *meta = lhg_arena_alloc(arena, sizeof *meta);
            memset(meta, 0, sizeof *meta);
            meta->name = group_name;
            meta->names = lhg_json_require(ctx, group, "names", group_name);
            meta->paths = lhg_json_require(ctx, group, "paths", group_name);
            for (int number = 1; number <= 4; number++)
                if (present_mask & (1 << number))
                    meta->discs[meta->disc_count++] = number;
            meta->catalog_sha1 = group_sha1;
            meta->catalog_size = group_size;
            meta->has_disc = distinct == 1;
            meta->disc = disc;
            meta->poll_forward = has_top && (top == 0 || top == 1);
            image->meta = meta;
            if (planned->hints) {
                size_t count = planned->hints->as.array.count;
                uint32_t *points = lhg_arena_alloc(arena, count * sizeof *points);
                for (size_t at = 0; at < count; at++) {
                    points[at] = parse_hex32(ctx, lhg_json_str(ctx, planned->hints->as.array.items[at], "entry_points"), "entry_points");
                    if (!lhg_entry_hint_valid(image, (int64_t)points[at]))
                        lhg_fail(ctx, "%s: entry point 0x%08lx is not an instruction of the image loaded from %s", group_name,
                                 (unsigned long)points[at], lhg_json_str(ctx, lhg_json_require(ctx, location, "path", "location"), "path"));
                }
                image->entry_points = points;
                image->entry_point_count = count;
            }
            size_t seed_count;
            uint32_t *seeds = lhg_prologue_seeds(arena, item->loaded, item->loaded_size, base, &seed_count);
            lhg_overlay_set *set = append_overlay(ctx, &result, &result_count, &result_capacity);
            set->index = index;
            set->image = image;
            /* Effect callbacks also appear as absolute pointers in data tables.
             * Requiring a function prologue keeps embedded pixels out of traversal. */
            lhg_discover(arena, image, seeds, seed_count, 0, &set->functions);
            if (assets_dir)
                lhg_write_file(ctx, lhg_fmt(arena, "%s/set%d.bin", assets_dir, index), item->loaded, item->loaded_size);
            item->owner->loaded_retained = 1;
            lhg_progress(ctx, "catalog", (unsigned long)result_count, (unsigned long)catalog->as.array.count);
        }
        for (size_t slot = 0; slot < decoded_count; slot++) {
            int shared = decoded[slot].data == decoded[slot].loaded;
            if (!decoded[slot].loaded_retained)
                lhg_free(ctx, decoded[slot].loaded);
            if (!shared)
                lhg_free(ctx, decoded[slot].data);
        }
        lhg_free(ctx, decoded);
        lhg_free(ctx, verified);
        lhg_free(ctx, chosen);
    }

    /* Code prefixes follow every group in the plan. */
    while (plan_next < plan_count && plan[plan_next].prefix) {
        const lhg_plan_row *row = &plan[plan_next++];
        compile_prefix(arena, row, discs, indexes, assets_dir, &result, &result_count, &result_capacity);
    }

    for (size_t index = 0; index < result_count; index++)
        for (size_t other = index + 1; other < result_count; other++)
            if (result[index].index == result[other].index)
                lhg_fail(ctx, "catalog set index collision");

    if (assets_dir) {
        lhg_text text;
        lhg_text_init(&text, ctx);
        lhg_json_dump(&text, evidence);
        lhg_text_adds(&text, "\n");
        lhg_write_file(ctx, lhg_fmt(arena, "%s/load-address-evidence.json", assets_dir), text.data, text.size);
        lhg_text_free(&text);
    }

    for (int number = 1; number <= 4; number++)
        if (present[number])
            lhg_ff8_image_close(&discs[number]);
    if (plan_next != plan_count)
        lhg_fail(ctx, "catalog plan lists images that were not verified");
    if (result_count)
        qsort(result, result_count, sizeof *result, compare_set);
    memset(program, 0, sizeof *program);
    program->main = main_image;
    program->overlays = result;
    program->overlay_count = result_count;
    program->plan = plan;
    program->plan_count = plan_count;
}
