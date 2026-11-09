/* Context, failure unwinding, allocators and text assembly shared by the generator. */
#ifndef LHG_BASE_H
#define LHG_BASE_H

#include <setjmp.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "lionheart_generator.h"

#if defined(__GNUC__) || defined(__clang__)
#define LHG_NORETURN __attribute__((noreturn))
#else
#define LHG_NORETURN
#endif

typedef union lhg_block lhg_block;
typedef struct lhg_open_file lhg_open_file;

/* All allocations and open files are tracked so a failure anywhere can unwind
 * to the public entry point without leaking; nothing outlives one generation. */
typedef struct {
    jmp_buf bail;
    lhg_status status;
    char error[1024];
    lhg_block *blocks;
    lhg_open_file *files;
    lhg_progress_fn progress;
    void *progress_user;
} lhg_ctx;

void lhg_ctx_init(lhg_ctx *ctx);
void lhg_ctx_release(lhg_ctx *ctx);
LHG_NORETURN void lhg_fail(lhg_ctx *ctx, const char *format, ...);
LHG_NORETURN void lhg_cancel(lhg_ctx *ctx);
void lhg_progress(lhg_ctx *ctx, const char *stage, unsigned long done, unsigned long total);

/* Registers an open file so a failure closes it; the platform layer uses these. */
FILE *lhg_track_file(lhg_ctx *ctx, FILE *file);
int lhg_untrack_file(lhg_ctx *ctx, FILE *file);

void *lhg_alloc(lhg_ctx *ctx, size_t size);
void *lhg_zalloc(lhg_ctx *ctx, size_t count, size_t size);
void *lhg_realloc(lhg_ctx *ctx, void *pointer, size_t size);
void lhg_free(lhg_ctx *ctx, void *pointer);
/* Ensures room for at least count + 1 elements in a growing array. */
void *lhg_grow(lhg_ctx *ctx, void *array, size_t *capacity, size_t count, size_t element);

typedef struct lhg_chunk lhg_chunk;
typedef struct {
    lhg_ctx *ctx;
    lhg_chunk *chunks;
    lhg_chunk *spare;
    char *cursor, *end;
} lhg_arena;

void lhg_arena_init(lhg_arena *arena, lhg_ctx *ctx);
void *lhg_arena_alloc(lhg_arena *arena, size_t size);
/* Keeps the chunks so the next unit reuses already-mapped memory. */
void lhg_arena_reset(lhg_arena *arena);
void lhg_arena_release(lhg_arena *arena);
char *lhg_arena_strdup(lhg_arena *arena, const char *text, size_t length);

/* Formatting. Directives, chosen to mirror the generator's own number forms:
 *   %s  C string             %d  int
 *   %D  int64_t decimal      %Q  uint64_t decimal
 *   %U  uint32_t as 0x????????u
 *   %X  uint32_t as ???????? (lowercase, 8 digits)
 *   %H  uint64_t as Python hex() (0x, lowercase, minimal digits)
 *   %%  percent sign
 */
const char *lhg_fmt(lhg_arena *arena, const char *format, ...);
const char *lhg_vfmt(lhg_arena *arena, size_t *length, const char *format, va_list arguments);

typedef struct {
    const char *text;
    uint32_t length;
} lhg_line;

typedef struct {
    lhg_arena *arena;
    lhg_line *items;
    uint32_t count, capacity;
} lhg_lines;

void lhg_lines_init(lhg_lines *lines, lhg_arena *arena);
void lhg_lines_add(lhg_lines *lines, const char *text);
void lhg_lines_addf(lhg_lines *lines, const char *format, ...);
void lhg_lines_extend(lhg_lines *lines, const lhg_lines *other);
void lhg_lines_replace(lhg_lines *lines, const char *from, const char *to);

typedef struct {
    lhg_ctx *ctx;
    char *data;
    size_t size, capacity;
} lhg_text;

void lhg_text_init(lhg_text *text, lhg_ctx *ctx);
void lhg_text_free(lhg_text *text);
void lhg_text_clear(lhg_text *text);
void lhg_text_add(lhg_text *text, const char *data, size_t length);
void lhg_text_adds(lhg_text *text, const char *data);
void lhg_text_addf(lhg_text *text, lhg_arena *scratch, const char *format, ...);
/* Appends the lines joined by newlines, with no trailing newline. */
void lhg_text_join(lhg_text *text, const lhg_lines *lines);

#endif
