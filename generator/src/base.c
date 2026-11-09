#include "base.h"

#include <stdlib.h>
#include <string.h>

union lhg_block {
    struct {
        lhg_block *previous, *next;
    } link;
    long double align;
};

void lhg_ctx_init(lhg_ctx *ctx)
{
    memset(ctx, 0, sizeof *ctx);
}

void lhg_fail(lhg_ctx *ctx, const char *format, ...)
{
    va_list arguments;
    va_start(arguments, format);
    vsnprintf(ctx->error, sizeof ctx->error, format, arguments);
    va_end(arguments);
    ctx->status = LHG_FAILED;
    longjmp(ctx->bail, 1);
}

void lhg_cancel(lhg_ctx *ctx)
{
    snprintf(ctx->error, sizeof ctx->error, "cancelled");
    ctx->status = LHG_CANCELLED;
    longjmp(ctx->bail, 1);
}

void lhg_progress(lhg_ctx *ctx, const char *stage, unsigned long done, unsigned long total)
{
    if (ctx->progress && ctx->progress(ctx->progress_user, stage, done, total))
        lhg_cancel(ctx);
}

void *lhg_alloc(lhg_ctx *ctx, size_t size)
{
    lhg_block *block = malloc(sizeof *block + (size ? size : 1));
    if (!block)
        lhg_fail(ctx, "out of memory");
    block->link.previous = NULL;
    block->link.next = ctx->blocks;
    if (ctx->blocks)
        ctx->blocks->link.previous = block;
    ctx->blocks = block;
    return block + 1;
}

void *lhg_zalloc(lhg_ctx *ctx, size_t count, size_t size)
{
    if (size && count > ((size_t)-1 - sizeof(lhg_block)) / size)
        lhg_fail(ctx, "out of memory");
    void *pointer = lhg_alloc(ctx, count * size);
    memset(pointer, 0, count * size);
    return pointer;
}

void *lhg_realloc(lhg_ctx *ctx, void *pointer, size_t size)
{
    if (!pointer)
        return lhg_alloc(ctx, size);
    lhg_block *block = (lhg_block *)pointer - 1;
    lhg_block *moved = realloc(block, sizeof *block + (size ? size : 1));
    if (!moved)
        lhg_fail(ctx, "out of memory");
    if (moved->link.previous)
        moved->link.previous->link.next = moved;
    else
        ctx->blocks = moved;
    if (moved->link.next)
        moved->link.next->link.previous = moved;
    return moved + 1;
}

void lhg_free(lhg_ctx *ctx, void *pointer)
{
    if (!pointer)
        return;
    lhg_block *block = (lhg_block *)pointer - 1;
    if (block->link.previous)
        block->link.previous->link.next = block->link.next;
    else
        ctx->blocks = block->link.next;
    if (block->link.next)
        block->link.next->link.previous = block->link.previous;
    free(block);
}

void *lhg_grow(lhg_ctx *ctx, void *array, size_t *capacity, size_t count, size_t element)
{
    if (count < *capacity)
        return array;
    size_t next = *capacity ? *capacity * 2 : 16;
    while (next <= count)
        next *= 2;
    if (next > ((size_t)-1 - sizeof(lhg_block)) / element)
        lhg_fail(ctx, "out of memory");
    *capacity = next;
    return lhg_realloc(ctx, array, next * element);
}

struct lhg_open_file {
    lhg_open_file *previous, *next;
    FILE *file;
};

void lhg_ctx_release(lhg_ctx *ctx)
{
    while (ctx->files) {
        lhg_open_file *entry = ctx->files;
        ctx->files = entry->next;
        fclose(entry->file);
        free(entry);
    }
    while (ctx->blocks) {
        lhg_block *block = ctx->blocks;
        ctx->blocks = block->link.next;
        free(block);
    }
}

FILE *lhg_track_file(lhg_ctx *ctx, FILE *file)
{
    lhg_open_file *entry = malloc(sizeof *entry);
    if (!entry) {
        fclose(file);
        lhg_fail(ctx, "out of memory");
    }
    entry->previous = NULL;
    entry->next = ctx->files;
    entry->file = file;
    if (ctx->files)
        ctx->files->previous = entry;
    ctx->files = entry;
    return file;
}

int lhg_untrack_file(lhg_ctx *ctx, FILE *file)
{
    for (lhg_open_file *entry = ctx->files; entry; entry = entry->next) {
        if (entry->file != file)
            continue;
        if (entry->previous)
            entry->previous->next = entry->next;
        else
            ctx->files = entry->next;
        if (entry->next)
            entry->next->previous = entry->previous;
        free(entry);
        return fclose(file);
    }
    return fclose(file);
}

struct lhg_chunk {
    lhg_chunk *next;
    size_t size;
};

enum { LHG_CHUNK_SIZE = 1 << 20 };

void lhg_arena_init(lhg_arena *arena, lhg_ctx *ctx)
{
    memset(arena, 0, sizeof *arena);
    arena->ctx = ctx;
}

static void arena_take_chunk(lhg_arena *arena, size_t size)
{
    lhg_chunk *chunk = NULL;
    for (lhg_chunk **slot = &arena->spare; *slot; slot = &(*slot)->next) {
        if ((*slot)->size >= size) {
            chunk = *slot;
            *slot = chunk->next;
            break;
        }
    }
    if (!chunk) {
        chunk = lhg_alloc(arena->ctx, sizeof *chunk + size);
        chunk->size = size;
    }
    chunk->next = arena->chunks;
    arena->chunks = chunk;
    arena->cursor = (char *)(chunk + 1);
    arena->end = arena->cursor + chunk->size;
}

void *lhg_arena_alloc(lhg_arena *arena, size_t size)
{
    size = (size + 7) & ~(size_t)7;
    if ((size_t)(arena->end - arena->cursor) < size)
        arena_take_chunk(arena, size > LHG_CHUNK_SIZE ? size : LHG_CHUNK_SIZE);
    void *pointer = arena->cursor;
    arena->cursor += size;
    return pointer;
}

void lhg_arena_reset(lhg_arena *arena)
{
    while (arena->chunks) {
        lhg_chunk *chunk = arena->chunks;
        arena->chunks = chunk->next;
        chunk->next = arena->spare;
        arena->spare = chunk;
    }
    arena->cursor = arena->end = NULL;
}

static void free_chunks(lhg_ctx *ctx, lhg_chunk *chunk)
{
    while (chunk) {
        lhg_chunk *next = chunk->next;
        lhg_free(ctx, chunk);
        chunk = next;
    }
}

void lhg_arena_release(lhg_arena *arena)
{
    free_chunks(arena->ctx, arena->chunks);
    free_chunks(arena->ctx, arena->spare);
    arena->chunks = arena->spare = NULL;
    arena->cursor = arena->end = NULL;
}

char *lhg_arena_strdup(lhg_arena *arena, const char *text, size_t length)
{
    char *copy = lhg_arena_alloc(arena, length + 1);
    memcpy(copy, text, length);
    copy[length] = 0;
    return copy;
}

static const char hex_digits[] = "0123456789abcdef";

static char *put_decimal(char *out, uint64_t value)
{
    char scratch[20];
    int used = 0;
    do {
        scratch[used++] = (char)('0' + value % 10);
        value /= 10;
    } while (value);
    while (used)
        *out++ = scratch[--used];
    return out;
}

static char *put_signed(char *out, int64_t value)
{
    if (value < 0) {
        *out++ = '-';
        return put_decimal(out, (uint64_t)0 - (uint64_t)value);
    }
    return put_decimal(out, (uint64_t)value);
}

static char *put_hex8(char *out, uint32_t value)
{
    for (int shift = 28; shift >= 0; shift -= 4)
        *out++ = hex_digits[(value >> shift) & 15];
    return out;
}

static char *put_py_hex(char *out, uint64_t value)
{
    *out++ = '0';
    *out++ = 'x';
    int shift = 60;
    while (shift > 0 && !((value >> shift) & 15))
        shift -= 4;
    for (; shift >= 0; shift -= 4)
        *out++ = hex_digits[(value >> shift) & 15];
    return out;
}

/* Sized in a first pass so every emitted string is one exact bump allocation. */
const char *lhg_vfmt(lhg_arena *arena, size_t *length, const char *format, va_list arguments)
{
    size_t need = 1;
    va_list measure;
    va_copy(measure, arguments);
    for (const char *f = format; *f; f++) {
        if (*f != '%') {
            need++;
            continue;
        }
        switch (*++f) {
        case 's': need += strlen(va_arg(measure, const char *)); break;
        case 'd': (void)va_arg(measure, int); need += 11; break;
        case 'D': (void)va_arg(measure, int64_t); need += 20; break;
        case 'Q': (void)va_arg(measure, uint64_t); need += 20; break;
        case 'U': (void)va_arg(measure, uint32_t); need += 11; break;
        case 'X': (void)va_arg(measure, uint32_t); need += 8; break;
        case 'H': (void)va_arg(measure, uint64_t); need += 18; break;
        case '%': need++; break;
        default: lhg_fail(arena->ctx, "internal error: bad format directive");
        }
    }
    va_end(measure);
    char *start = lhg_arena_alloc(arena, need);
    char *out = start;
    for (const char *f = format; *f; f++) {
        if (*f != '%') {
            *out++ = *f;
            continue;
        }
        switch (*++f) {
        case 's': {
            const char *text = va_arg(arguments, const char *);
            size_t size = strlen(text);
            memcpy(out, text, size);
            out += size;
            break;
        }
        case 'd': out = put_signed(out, va_arg(arguments, int)); break;
        case 'D': out = put_signed(out, va_arg(arguments, int64_t)); break;
        case 'Q': out = put_decimal(out, va_arg(arguments, uint64_t)); break;
        case 'U':
            *out++ = '0';
            *out++ = 'x';
            out = put_hex8(out, va_arg(arguments, uint32_t));
            *out++ = 'u';
            break;
        case 'X': out = put_hex8(out, va_arg(arguments, uint32_t)); break;
        case 'H': out = put_py_hex(out, va_arg(arguments, uint64_t)); break;
        case '%': *out++ = '%'; break;
        }
    }
    *out = 0;
    if (length)
        *length = (size_t)(out - start);
    /* Return the unused tail of the upper-bound reservation. */
    arena->cursor = start + (((size_t)(out - start) + 8) & ~(size_t)7);
    return start;
}

const char *lhg_fmt(lhg_arena *arena, const char *format, ...)
{
    va_list arguments;
    va_start(arguments, format);
    const char *result = lhg_vfmt(arena, NULL, format, arguments);
    va_end(arguments);
    return result;
}

void lhg_lines_init(lhg_lines *lines, lhg_arena *arena)
{
    lines->arena = arena;
    lines->items = NULL;
    lines->count = lines->capacity = 0;
}

static void lines_reserve(lhg_lines *lines, uint32_t extra)
{
    if (lines->count + extra <= lines->capacity)
        return;
    uint32_t next = lines->capacity ? lines->capacity * 2 : 16;
    while (next < lines->count + extra)
        next *= 2;
    lhg_line *grown = lhg_arena_alloc(lines->arena, next * sizeof *grown);
    if (lines->count)
        memcpy(grown, lines->items, lines->count * sizeof *grown);
    lines->items = grown;
    lines->capacity = next;
}

static void lines_push(lhg_lines *lines, const char *text, size_t length)
{
    lines_reserve(lines, 1);
    lines->items[lines->count].text = text;
    lines->items[lines->count].length = (uint32_t)length;
    lines->count++;
}

void lhg_lines_add(lhg_lines *lines, const char *text)
{
    lines_push(lines, text, strlen(text));
}

void lhg_lines_addf(lhg_lines *lines, const char *format, ...)
{
    size_t length;
    va_list arguments;
    va_start(arguments, format);
    const char *text = lhg_vfmt(lines->arena, &length, format, arguments);
    va_end(arguments);
    lines_push(lines, text, length);
}

void lhg_lines_extend(lhg_lines *lines, const lhg_lines *other)
{
    if (!other->count)
        return;
    lines_reserve(lines, other->count);
    memcpy(lines->items + lines->count, other->items, other->count * sizeof *other->items);
    lines->count += other->count;
}

void lhg_lines_replace(lhg_lines *lines, const char *from, const char *to)
{
    size_t from_length = strlen(from), to_length = strlen(to);
    for (uint32_t index = 0; index < lines->count; index++) {
        lhg_line *line = &lines->items[index];
        const char *hit = strstr(line->text, from);
        if (!hit)
            continue;
        size_t matches = 0;
        for (const char *at = hit; at; at = strstr(at + from_length, from))
            matches++;
        size_t length = line->length + matches * to_length - matches * from_length;
        char *rewritten = lhg_arena_alloc(lines->arena, length + 1);
        char *out = rewritten;
        const char *source = line->text;
        for (const char *at = hit; at; at = strstr(source, from)) {
            memcpy(out, source, (size_t)(at - source));
            out += at - source;
            memcpy(out, to, to_length);
            out += to_length;
            source = at + from_length;
        }
        size_t tail = line->length - (size_t)(source - line->text);
        memcpy(out, source, tail);
        out[tail] = 0;
        line->text = rewritten;
        line->length = (uint32_t)length;
    }
}

void lhg_text_init(lhg_text *text, lhg_ctx *ctx)
{
    memset(text, 0, sizeof *text);
    text->ctx = ctx;
}

void lhg_text_free(lhg_text *text)
{
    lhg_free(text->ctx, text->data);
    memset(text, 0, sizeof *text);
}

void lhg_text_clear(lhg_text *text)
{
    text->size = 0;
}

static void text_reserve(lhg_text *text, size_t extra)
{
    if (text->size + extra <= text->capacity)
        return;
    size_t next = text->capacity ? text->capacity : 1 << 16;
    while (next < text->size + extra)
        next *= 2;
    text->data = lhg_realloc(text->ctx, text->data, next);
    text->capacity = next;
}

void lhg_text_add(lhg_text *text, const char *data, size_t length)
{
    if (!length)
        return;
    text_reserve(text, length);
    memcpy(text->data + text->size, data, length);
    text->size += length;
}

void lhg_text_adds(lhg_text *text, const char *data)
{
    lhg_text_add(text, data, strlen(data));
}

void lhg_text_addf(lhg_text *text, lhg_arena *scratch, const char *format, ...)
{
    size_t length;
    va_list arguments;
    va_start(arguments, format);
    const char *data = lhg_vfmt(scratch, &length, format, arguments);
    va_end(arguments);
    lhg_text_add(text, data, length);
}

void lhg_text_join(lhg_text *text, const lhg_lines *lines)
{
    size_t total = lines->count;
    for (uint32_t index = 0; index < lines->count; index++)
        total += lines->items[index].length;
    text_reserve(text, total);
    char *out = text->data + text->size;
    for (uint32_t index = 0; index < lines->count; index++) {
        const lhg_line *line = &lines->items[index];
        if (index)
            *out++ = '\n';
        memcpy(out, line->text, line->length);
        out += line->length;
    }
    text->size = (size_t)(out - text->data);
}
