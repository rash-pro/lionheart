/* Host file access. Paths are UTF-8 everywhere; content is binary with LF so
 * generated text is byte-identical on Windows and Linux. */
#ifndef LHG_PLATFORM_H
#define LHG_PLATFORM_H

#include <stdint.h>

#include "base.h"

FILE *lhg_file_open(lhg_ctx *ctx, const char *path, const char *mode);
FILE *lhg_file_try_open(lhg_ctx *ctx, const char *path, const char *mode);
void lhg_file_close(lhg_ctx *ctx, FILE *file);
void lhg_file_seek(lhg_ctx *ctx, FILE *file, int64_t offset, int whence);
int64_t lhg_file_size(lhg_ctx *ctx, FILE *file);

/* Reads a whole file into a tracked buffer with a terminating NUL past size. */
uint8_t *lhg_read_file(lhg_ctx *ctx, const char *path, size_t *size);
void lhg_write_file(lhg_ctx *ctx, const char *path, const void *data, size_t size);
/* Skips the write when the content already matches, so build systems keep timestamps. */
void lhg_write_if_changed(lhg_ctx *ctx, const char *path, const void *data, size_t size);
void lhg_make_directories(lhg_ctx *ctx, const char *path);

typedef void (*lhg_dir_visit)(lhg_ctx *ctx, void *user, const char *name);
/* Visits every entry name in a directory except "." and "..". */
void lhg_list_directory(lhg_ctx *ctx, const char *path, lhg_dir_visit visit, void *user);
void lhg_remove_file(lhg_ctx *ctx, const char *path);
int lhg_is_separator(char c);
const char *lhg_join_path(lhg_arena *arena, const char *parent, const char *name);
/* The directory containing path; empty when path has no directory part. */
const char *lhg_parent_directory(lhg_arena *arena, const char *path);

#endif
