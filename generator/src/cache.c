/* Cache policy lives beside generation so an embedding host shares the CLI's
 * input identity and interruption rules. The receipt is a commit marker: it is
 * removed before emission and atomically replaced only after all writes close. */
#include "cache.h"
#include "generator_build.h"
#include "platform.h"
#include "sha256.h"
#include <stdlib.h>
#include <string.h>

const char *lhg_generator_fingerprint(void)
{
    return LHG_FINGERPRINT;
}

static void add_string(lhg_sha256 *hash, const char *value)
{
    lhg_sha256_add(hash, value, strlen(value) + 1);
}
static void add_number(lhg_sha256 *hash, lhg_arena *arena, int64_t value)
{
    add_string(hash, lhg_fmt(arena, "%D", value));
}

typedef struct {
    lhg_arena *arena;
    const char **names;
    size_t count, capacity;
    int catalogs;
} names;

static int output_name(const char *name)
{
    if (!strcmp(name, "generated.h") || !strcmp(name, "dispatch_table.c") || !strcmp(name, "manifest.json") ||
        !strcmp(name, "profile-catalog.txt") || !strcmp(name, "sources.cmake"))
        return 1;
    size_t size = strlen(name);
    return size > 2 && !strcmp(name + size - 2, ".c") && (!strncmp(name, "functions_", 10) || !strncmp(name, "overlay_", 8));
}
static void collect(lhg_ctx *ctx, void *user, const char *name)
{
    names *list = user;
    size_t size = strlen(name);
    if (list->catalogs ? size < 5 || strcmp(name + size - 5, ".json") : !output_name(name))
        return;
    list->names = lhg_grow(ctx, list->names, &list->capacity, list->count, sizeof *list->names);
    list->names[list->count++] = lhg_arena_strdup(list->arena, name, size);
}
static int compare_names(const void *a, const void *b)
{
    return strcmp(*(const char *const *)a, *(const char *const *)b);
}
static void list_names(names *list, lhg_arena *arena, const char *directory, int catalogs)
{
    memset(list, 0, sizeof *list);
    list->arena = arena;
    list->catalogs = catalogs;
    lhg_list_directory(arena->ctx, directory, collect, list);
    if (list->count > 1)
        qsort(list->names, list->count, sizeof *list->names, compare_names);
}
static int file_digest(lhg_ctx *ctx, const char *path, char hex[65])
{
    FILE *file = lhg_file_try_open(ctx, path, "rb");
    if (!file)
        return 0;
    lhg_sha256 hash;
    lhg_sha256_init(&hash);
    uint8_t buffer[16384];
    size_t size;
    while ((size = fread(buffer, 1, sizeof buffer, file)) != 0)
        lhg_sha256_add(&hash, buffer, size);
    int ok = !ferror(file);
    lhg_file_close(ctx, file);
    lhg_sha256_finish(&hash, hex);
    return ok;
}
void lhg_cache_key(lhg_arena *arena, const lhg_options *options, const lhg_program *program, char key[65])
{
    lhg_sha256 hash;
    lhg_sha256_init(&hash);
    add_string(&hash, "lionheart-native-setup-v1");
    add_string(&hash, lhg_generator_fingerprint());
    names catalogs;
    list_names(&catalogs, arena, options->catalog_dir, 1);
    for (size_t i = 0; i < catalogs.count; i++) {
        char hex[65];
        const char *name = catalogs.names[i];
        if (!file_digest(arena->ctx, lhg_join_path(arena, options->catalog_dir, name), hex))
            lhg_fail(arena->ctx, "cannot fingerprint catalog %s", name);
        add_string(&hash, name);
        add_string(&hash, hex);
    }
    lhg_free(arena->ctx, catalogs.names);
    add_number(&hash, arena, options->mode);
    add_number(&hash, arena, options->host_mode);
    add_number(&hash, arena, options->timing);
    add_number(&hash, arena, options->legacy_overlays);
    add_number(&hash, arena, program->disc_mask);
    lhg_sha256_add(&hash, program->main->data, program->main->size);
    for (size_t i = 0; i < program->overlay_count; i++) {
        const lhg_overlay_set *set = &program->overlays[i];
        add_number(&hash, arena, set->index);
        add_number(&hash, arena, (int64_t)set->image->size);
        lhg_sha256_add(&hash, set->image->data, set->image->size);
    }
    const lhg_register_cache *selection = options->register_cache;
    add_number(&hash, arena, selection != NULL);
    if (selection) {
        add_number(&hash, arena, selection->max_registers);
        add_number(&hash, arena, (int64_t)selection->function_count);
        for (size_t i = 0; i < selection->function_count; i++)
            add_string(&hash, selection->functions[i]);
        add_number(&hash, arena, (int64_t)selection->fuse_forward_count);
        for (size_t i = 0; i < selection->fuse_forward_count; i++)
            add_string(&hash, selection->fuse_forward[i]);
    }
    lhg_sha256_finish(&hash, key);
}

static const char *receipt_path(lhg_arena *arena, const char *output)
{
    return lhg_join_path(arena, output, "setup-cache.native");
}
static void receipt(lhg_arena *arena, const char *output, const char *key, lhg_text *text)
{
    lhg_text_adds(text, "lionheart-native-setup-v1\n");
    lhg_text_adds(text, key);
    lhg_text_adds(text, "\n");
    names files;
    list_names(&files, arena, output, 0);
    /* These five files are emitted even for the smallest catalog. */
    const char *required[] = {"generated.h", "dispatch_table.c", "manifest.json", "profile-catalog.txt", "sources.cmake"};
    for (size_t i = 0; i < sizeof required / sizeof *required; i++) {
        size_t j = 0;
        while (j < files.count && strcmp(files.names[j], required[i]))
            j++;
        if (j == files.count) {
            lhg_text_clear(text);
            lhg_free(arena->ctx, files.names);
            return;
        }
    }
    for (size_t i = 0; i < files.count; i++) {
        char hex[65];
        if (!file_digest(arena->ctx, lhg_join_path(arena, output, files.names[i]), hex)) {
            lhg_text_clear(text);
            break;
        }
        lhg_text_adds(text, hex);
        lhg_text_adds(text, " ");
        lhg_text_adds(text, files.names[i]);
        lhg_text_adds(text, "\n");
    }
    lhg_free(arena->ctx, files.names);
}
int lhg_output_cache_hit(lhg_arena *arena, const char *output, const char *key)
{
    FILE *file = lhg_file_try_open(arena->ctx, receipt_path(arena, output), "rb");
    if (!file)
        return 0;
    lhg_text expected;
    lhg_text_init(&expected, arena->ctx);
    receipt(arena, output, key, &expected);
    int hit = expected.size != 0;
    uint8_t buffer[16384];
    size_t offset = 0, size;
    while ((size = fread(buffer, 1, sizeof buffer, file)) != 0) {
        if (offset + size > expected.size || memcmp(buffer, expected.data + offset, size)) {
            hit = 0;
            break;
        }
        offset += size;
    }
    hit = hit && !ferror(file) && offset == expected.size;
    lhg_file_close(arena->ctx, file);
    lhg_text_free(&expected);
    return hit;
}
void lhg_output_cache_invalidate(lhg_arena *arena, const char *output)
{
    lhg_remove_file(arena->ctx, receipt_path(arena, output));
}
void lhg_output_cache_save(lhg_arena *arena, const char *output, const char *key)
{
    lhg_text text;
    lhg_text_init(&text, arena->ctx);
    receipt(arena, output, key, &text);
    if (!text.size)
        lhg_fail(arena->ctx, "cannot verify generated outputs");
    const char *temporary = lhg_join_path(arena, output, "setup-cache.native.tmp");
    lhg_write_file(arena->ctx, temporary, text.data, text.size);
    lhg_replace_file(arena->ctx, temporary, receipt_path(arena, output));
    lhg_text_free(&text);
}
