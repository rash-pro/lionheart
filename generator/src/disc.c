#include "disc.h"

#include <string.h>

#include "platform.h"

enum { RAW_SECTOR = 2352, DATA_OFFSET = 24, USER_SECTOR = 2048, MAX_DIRECTORY_DEPTH = 32 };

/* Python str.splitlines() also breaks on these ASCII controls. */
static int is_line_break(char c)
{
    return c == '\n' || c == '\r' || c == '\v' || c == '\f' || c == '\x1c' || c == '\x1d' || c == '\x1e';
}

static int is_space(char c)
{
    return c == ' ' || (c >= '\t' && c <= '\r') || (c >= '\x1c' && c <= '\x1f');
}

static const char *cue_track_path(lhg_ctx *ctx, lhg_arena *arena, const char *cue_path)
{
    size_t size;
    const char *text = (const char *)lhg_read_file(ctx, cue_path, &size);
    const char *end = text + size;
    for (const char *line = text; line < end;) {
        const char *stop = line;
        while (stop < end && !is_line_break(*stop))
            stop++;
        const char *at = line;
        while (at < stop && is_space(*at))
            at++;
        if (stop - at >= 4 && !memcmp(at, "FILE", 4)) {
            const char *open = memchr(line, '"', (size_t)(stop - line));
            if (!open)
                lhg_fail(ctx, "%s: FILE line has no quoted name", cue_path);
            const char *close = memchr(open + 1, '"', (size_t)(stop - open - 1));
            if (!close)
                close = stop;
            const char *parent = lhg_parent_directory(arena, cue_path);
            return lhg_join_path(arena, parent, lhg_arena_strdup(arena, open + 1, (size_t)(close - open - 1)));
        }
        line = stop + 1;
    }
    lhg_fail(ctx, "%s: no FILE line", cue_path);
}

/* Returns the user bytes actually present in a sector; a short final sector is tolerated. */
static size_t read_sector(lhg_disc *disc, uint32_t lba, uint8_t sector[USER_SECTOR])
{
    lhg_file_seek(disc->ctx, disc->bin, (int64_t)lba * RAW_SECTOR, SEEK_SET);
    uint8_t raw[RAW_SECTOR];
    size_t got = fread(raw, 1, RAW_SECTOR, disc->bin);
    if (got <= DATA_OFFSET)
        return 0;
    size_t available = got - DATA_OFFSET;
    if (available > USER_SECTOR)
        available = USER_SECTOR;
    memcpy(sector, raw + DATA_OFFSET, available);
    return available;
}

uint8_t *lhg_disc_read(lhg_disc *disc, uint32_t lba, int64_t size)
{
    if (size < 0)
        lhg_fail(disc->ctx, "disc read with a negative size");
    /* Checked before allocating: a corrupt size must not become a giant buffer. */
    int64_t last_sector = (int64_t)lba + (size + USER_SECTOR - 1) / USER_SECTOR;
    if (last_sector * RAW_SECTOR > disc->bin_size + RAW_SECTOR)
        lhg_fail(disc->ctx, "truncated disc image");
    uint8_t *data = lhg_alloc(disc->ctx, (size_t)size + 1);
    size_t used = 0;
    while (used < (size_t)size) {
        uint8_t sector[USER_SECTOR];
        size_t available = read_sector(disc, lba++, sector);
        if (!available)
            lhg_fail(disc->ctx, "truncated disc image");
        size_t take = (size_t)size - used;
        if (take > available)
            take = available;
        memcpy(data + used, sector, take);
        used += take;
    }
    data[used] = 0;
    return data;
}

static lhg_iso_record parse_record(lhg_disc *disc, const uint8_t *raw, size_t size)
{
    if (size < 33)
        lhg_fail(disc->ctx, "corrupt ISO directory record");
    lhg_iso_record record;
    record.lba = (uint32_t)raw[2] | (uint32_t)raw[3] << 8 | (uint32_t)raw[4] << 16 | (uint32_t)raw[5] << 24;
    record.size = (uint32_t)raw[10] | (uint32_t)raw[11] << 8 | (uint32_t)raw[12] << 16 | (uint32_t)raw[13] << 24;
    record.directory = (raw[25] & 2) != 0;
    size_t name_size = raw[32];
    const uint8_t *name = raw + 33;
    if (33 + name_size > size)
        name_size = size - 33;
    size_t stop = 0;
    while (stop < name_size && name[stop] != ';')
        stop++;
    /* A single 0x00 or 0x01 byte names the directory's self and parent links. */
    record.link = stop == 1 && name[0] <= 1;
    char *out = lhg_arena_alloc(disc->arena, stop * 3 + 1);
    char *to = out;
    for (size_t index = 0; index < stop; index++) {
        if (name[index] < 0x80) {
            *to++ = (char)name[index];
        } else {
            /* Non-ASCII bytes become U+FFFD, as the ISO decoder does. */
            *to++ = (char)0xEF;
            *to++ = (char)0xBF;
            *to++ = (char)0xBD;
        }
    }
    *to = 0;
    record.name = out;
    return record;
}

static void walk_directory(lhg_disc *disc, lhg_iso_record entry, const char *prefix, int depth)
{
    lhg_ctx *ctx = disc->ctx;
    if (depth > MAX_DIRECTORY_DEPTH)
        lhg_fail(ctx, "corrupt ISO directory tree");
    uint8_t *data = lhg_disc_read(disc, entry.lba, (int64_t)entry.size);
    size_t position = 0, size = entry.size;
    while (position < size) {
        unsigned length = data[position];
        if (!length) {
            /* Records never cross a sector boundary. */
            position = (position / USER_SECTOR + 1) * USER_SECTOR;
            continue;
        }
        if (position + length > size)
            lhg_fail(ctx, "corrupt ISO directory record");
        lhg_iso_record record = parse_record(disc, data + position, length);
        position += length;
        if (record.link)
            continue;
        const char *path = lhg_fmt(disc->arena, "%s/%s", prefix, record.name);
        disc->files = lhg_grow(ctx, disc->files, &disc->file_capacity, disc->file_count, sizeof *disc->files);
        disc->files[disc->file_count].path = path;
        disc->files[disc->file_count++].record = record;
        if (record.directory)
            walk_directory(disc, record, path, depth + 1);
    }
    lhg_free(ctx, data);
}

void lhg_disc_open(lhg_disc *disc, lhg_arena *arena, const char *cue_path)
{
    memset(disc, 0, sizeof *disc);
    disc->ctx = arena->ctx;
    disc->arena = arena;
    const char *bin_path = cue_track_path(disc->ctx, arena, cue_path);
    disc->bin = lhg_file_open(disc->ctx, bin_path, "rb");
    disc->bin_size = lhg_file_size(disc->ctx, disc->bin);
    /* Sector 16 holds the volume descriptor; its root record is at offset 156. */
    uint8_t sector[USER_SECTOR];
    size_t available = read_sector(disc, 16, sector);
    if (available < 156 + 34)
        lhg_fail(disc->ctx, "%s: truncated disc image", cue_path);
    disc->root = parse_record(disc, sector + 156, 34);
    walk_directory(disc, disc->root, "", 0);
}

void lhg_disc_close(lhg_disc *disc)
{
    if (disc->bin)
        lhg_file_close(disc->ctx, disc->bin);
    disc->bin = NULL;
}

static int equal_ignoring_case(const char *a, const char *b)
{
    for (;; a++, b++) {
        char x = (*a >= 'a' && *a <= 'z') ? (char)(*a - 32) : *a;
        char y = (*b >= 'a' && *b <= 'z') ? (char)(*b - 32) : *b;
        if (x != y)
            return 0;
        if (!x)
            return 1;
    }
}

const lhg_disc_file *lhg_disc_find(lhg_disc *disc, const char *path)
{
    size_t size = strlen(path);
    while (size && path[size - 1] == '/')
        size--;
    while (size && *path == '/') {
        path++;
        size--;
    }
    char *want = lhg_arena_strdup(disc->arena, path, size);
    for (size_t index = 0; index < disc->file_count; index++) {
        const char *candidate = disc->files[index].path;
        while (*candidate == '/')
            candidate++;
        if (equal_ignoring_case(candidate, want))
            return &disc->files[index];
    }
    lhg_fail(disc->ctx, "file not found on disc: %s", path);
}

static int matches_ff8_image(const char *path)
{
    return strlen(path) == 13 && !memcmp(path, "/FF8DISC", 8) && path[8] >= '1' && path[8] <= '4' &&
           !memcmp(path + 9, ".IMG", 4);
}

void lhg_ff8_image_open(lhg_ff8_image *image, lhg_arena *arena, const char *cue_path)
{
    lhg_ctx *ctx = arena->ctx;
    memset(image, 0, sizeof *image);
    lhg_disc_open(&image->disc, arena, cue_path);
    const lhg_disc_file *found = NULL;
    size_t matches = 0;
    for (size_t index = 0; index < image->disc.file_count; index++) {
        if (matches_ff8_image(image->disc.files[index].path)) {
            found = &image->disc.files[index];
            matches++;
        }
    }
    if (matches != 1)
        lhg_fail(ctx, "expected exactly one FF8DISC[1-4].IMG");
    image->record = found->record;
    image->number = found->path[8] - '0';
    if (image->record.size % USER_SECTOR)
        lhg_fail(ctx, "IMG size is not sector aligned");
    if (((int64_t)image->record.lba + image->record.size / USER_SECTOR) * RAW_SECTOR > image->disc.bin_size)
        lhg_fail(ctx, "truncated disc image");
    uint8_t *table = lhg_ff8_image_read(image, 0, USER_SECTOR);
    int top = 0;
    for (int index = 0; index < USER_SECTOR / 8; index++) {
        const uint8_t *row = table + index * 8;
        if (!(row[0] | row[1] | row[2] | row[3] | row[4] | row[5] | row[6] | row[7]))
            break;
        top++;
    }
    lhg_free(ctx, table);
    if (top != 133)
        lhg_fail(ctx, "expected 133 top-table entries, got %d", top);
}

void lhg_ff8_image_close(lhg_ff8_image *image)
{
    lhg_disc_close(&image->disc);
}

uint8_t *lhg_ff8_image_read(lhg_ff8_image *image, int64_t sector, int64_t size)
{
    lhg_ctx *ctx = image->disc.ctx;
    if (sector < 0 || size < 0 || sector * USER_SECTOR + size > (int64_t)image->record.size)
        lhg_fail(ctx, "IMG read outside image: sector=%lld, size=%lld", (long long)sector, (long long)size);
    return lhg_disc_read(&image->disc, image->record.lba + (uint32_t)sector, size);
}
