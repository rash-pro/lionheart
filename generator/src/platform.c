/* Feature macros must come first: C99 hides these POSIX calls otherwise. */
#define _POSIX_C_SOURCE 200809L
#define _FILE_OFFSET_BITS 64

#include "platform.h"

#include <errno.h>
#include <string.h>

#ifdef _WIN32
#include <direct.h>
#include <io.h>
#include <wchar.h>
#include <windows.h>
#else
#include <dirent.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#endif

#ifdef _WIN32
static wchar_t *wide_path(lhg_ctx *ctx, const char *path)
{
    int size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, NULL, 0);
    if (size <= 0)
        lhg_fail(ctx, "invalid UTF-8 in path: %s", path);
    wchar_t *wide = lhg_alloc(ctx, (size_t)size * sizeof *wide);
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, wide, size);
    return wide;
}
#endif

FILE *lhg_file_try_open(lhg_ctx *ctx, const char *path, const char *mode)
{
#ifdef _WIN32
    wchar_t *wide = wide_path(ctx, path);
    wchar_t wide_mode[8];
    size_t used = 0;
    for (; mode[used] && used + 1 < 8; used++)
        wide_mode[used] = (wchar_t)mode[used];
    wide_mode[used] = 0;
    FILE *file = _wfopen(wide, wide_mode);
    lhg_free(ctx, wide);
#else
    FILE *file = fopen(path, mode);
#endif
    return file ? lhg_track_file(ctx, file) : NULL;
}

FILE *lhg_file_open(lhg_ctx *ctx, const char *path, const char *mode)
{
    FILE *file = lhg_file_try_open(ctx, path, mode);
    if (!file)
        lhg_fail(ctx, "cannot open %s: %s", path, strerror(errno));
    return file;
}

void lhg_file_close(lhg_ctx *ctx, FILE *file)
{
    if (file && lhg_untrack_file(ctx, file))
        lhg_fail(ctx, "cannot finish writing a file: %s", strerror(errno));
}

void lhg_file_seek(lhg_ctx *ctx, FILE *file, int64_t offset, int whence)
{
#ifdef _WIN32
    int failed = _fseeki64(file, offset, whence);
#else
    int failed = fseeko(file, (off_t)offset, whence);
#endif
    if (failed)
        lhg_fail(ctx, "seek failed: %s", strerror(errno));
}

int64_t lhg_file_size(lhg_ctx *ctx, FILE *file)
{
    lhg_file_seek(ctx, file, 0, SEEK_END);
#ifdef _WIN32
    int64_t size = _ftelli64(file);
#else
    int64_t size = (int64_t)ftello(file);
#endif
    if (size < 0)
        lhg_fail(ctx, "cannot determine a file size: %s", strerror(errno));
    lhg_file_seek(ctx, file, 0, SEEK_SET);
    return size;
}

uint8_t *lhg_read_file(lhg_ctx *ctx, const char *path, size_t *size)
{
    FILE *file = lhg_file_open(ctx, path, "rb");
    int64_t length = lhg_file_size(ctx, file);
    uint8_t *data = lhg_alloc(ctx, (size_t)length + 1);
    if (length && fread(data, 1, (size_t)length, file) != (size_t)length)
        lhg_fail(ctx, "cannot read %s", path);
    data[length] = 0;
    lhg_file_close(ctx, file);
    *size = (size_t)length;
    return data;
}

void lhg_write_file(lhg_ctx *ctx, const char *path, const void *data, size_t size)
{
    FILE *file = lhg_file_open(ctx, path, "wb");
    if (size && fwrite(data, 1, size, file) != size)
        lhg_fail(ctx, "cannot write %s: %s", path, strerror(errno));
    lhg_file_close(ctx, file);
}

void lhg_write_if_changed(lhg_ctx *ctx, const char *path, const void *data, size_t size)
{
    FILE *existing = lhg_file_try_open(ctx, path, "rb");
    if (existing) {
        int64_t length = lhg_file_size(ctx, existing);
        int same = 0;
        if (length == (int64_t)size) {
            uint8_t *current = lhg_alloc(ctx, size + 1);
            same = !size || (fread(current, 1, size, existing) == size && !memcmp(current, data, size));
            lhg_free(ctx, current);
        }
        lhg_file_close(ctx, existing);
        if (same)
            return;
    }
    lhg_write_file(ctx, path, data, size);
}

int lhg_is_separator(char c)
{
#ifdef _WIN32
    return c == '/' || c == '\\';
#else
    return c == '/';
#endif
}

static int make_one_directory(lhg_ctx *ctx, const char *path)
{
#ifdef _WIN32
    wchar_t *wide = wide_path(ctx, path);
    int result = _wmkdir(wide);
    lhg_free(ctx, wide);
    return result;
#else
    (void)ctx;
    return mkdir(path, 0777);
#endif
}

/* Intermediate components may be unreadable roots ("C:", "\\\\server") that
 * refuse mkdir while still existing; only the full path decides success. */
void lhg_make_directories(lhg_ctx *ctx, const char *path)
{
    size_t length = strlen(path);
    char *scratch = lhg_alloc(ctx, length + 1);
    memcpy(scratch, path, length + 1);
    for (size_t index = 1; index <= length; index++) {
        if (index != length && !lhg_is_separator(scratch[index]))
            continue;
        char saved = scratch[index];
        scratch[index] = 0;
        if (scratch[0] && make_one_directory(ctx, scratch) && index == length && errno != EEXIST)
            lhg_fail(ctx, "cannot create %s: %s", scratch, strerror(errno));
        scratch[index] = saved;
    }
    lhg_free(ctx, scratch);
}

void lhg_list_directory(lhg_ctx *ctx, const char *path, lhg_dir_visit visit, void *user)
{
#ifdef _WIN32
    lhg_arena arena;
    lhg_arena_init(&arena, ctx);
    wchar_t *pattern = wide_path(ctx, lhg_fmt(&arena, "%s\\*", path));
    WIN32_FIND_DATAW found;
    HANDLE handle = FindFirstFileW(pattern, &found);
    lhg_free(ctx, pattern);
    if (handle == INVALID_HANDLE_VALUE) {
        lhg_arena_release(&arena);
        lhg_fail(ctx, "cannot list %s", path);
    }
    do {
        if (!wcscmp(found.cFileName, L".") || !wcscmp(found.cFileName, L".."))
            continue;
        char name[1024];
        if (WideCharToMultiByte(CP_UTF8, 0, found.cFileName, -1, name, sizeof name, NULL, NULL) > 0)
            visit(ctx, user, name);
    } while (FindNextFileW(handle, &found));
    FindClose(handle);
    lhg_arena_release(&arena);
#else
    DIR *directory = opendir(path);
    if (!directory)
        lhg_fail(ctx, "cannot list %s: %s", path, strerror(errno));
    /* visit may fail, so copy names out first and only then call back. */
    char **names = NULL;
    size_t count = 0, capacity = 0;
    for (struct dirent *entry; (entry = readdir(directory)) != NULL;) {
        if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, ".."))
            continue;
        size_t size = strlen(entry->d_name);
        char *copy = lhg_alloc(ctx, size + 1);
        memcpy(copy, entry->d_name, size + 1);
        names = lhg_grow(ctx, names, &capacity, count, sizeof *names);
        names[count++] = copy;
    }
    closedir(directory);
    for (size_t index = 0; index < count; index++) {
        visit(ctx, user, names[index]);
        lhg_free(ctx, names[index]);
    }
    lhg_free(ctx, names);
#endif
}

void lhg_remove_file(lhg_ctx *ctx, const char *path)
{
#ifdef _WIN32
    wchar_t *wide = wide_path(ctx, path);
    int failed = _wunlink(wide);
    lhg_free(ctx, wide);
#else
    int failed = unlink(path);
#endif
    if (failed)
        lhg_fail(ctx, "cannot remove %s: %s", path, strerror(errno));
}

const char *lhg_join_path(lhg_arena *arena, const char *parent, const char *name)
{
    if (!*parent || lhg_is_separator(name[0]))
        return lhg_fmt(arena, "%s", name);
#ifdef _WIN32
    if (name[0] && name[1] == ':')
        return lhg_fmt(arena, "%s", name);
#endif
    if (lhg_is_separator(parent[strlen(parent) - 1]))
        return lhg_fmt(arena, "%s%s", parent, name);
    return lhg_fmt(arena, "%s/%s", parent, name);
}

const char *lhg_parent_directory(lhg_arena *arena, const char *path)
{
    size_t length = strlen(path);
    while (length && !lhg_is_separator(path[length - 1]))
        length--;
    while (length > 1 && lhg_is_separator(path[length - 1]))
        length--;
    return lhg_arena_strdup(arena, path, length);
}
