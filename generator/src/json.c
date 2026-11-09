#include "json.h"

#include <stdlib.h>
#include <string.h>

typedef struct {
    lhg_arena *arena;
    lhg_ctx *ctx;
    const char *start, *cursor, *end;
    const char *name;
    int depth;
} parser;

LHG_NORETURN static void parse_error(parser *p, const char *why)
{
    lhg_fail(p->ctx, "%s: invalid JSON at byte %lu: %s", p->name, (unsigned long)(p->cursor - p->start), why);
}

static void skip_space(parser *p)
{
    while (p->cursor < p->end && (*p->cursor == ' ' || *p->cursor == '\n' || *p->cursor == '\r' || *p->cursor == '\t'))
        p->cursor++;
}

static unsigned hex_value(parser *p, char c)
{
    if (c >= '0' && c <= '9')
        return (unsigned)(c - '0');
    if (c >= 'a' && c <= 'f')
        return (unsigned)(c - 'a' + 10);
    if (c >= 'A' && c <= 'F')
        return (unsigned)(c - 'A' + 10);
    parse_error(p, "bad unicode escape");
}

static unsigned parse_u_escape(parser *p)
{
    if (p->end - p->cursor < 4)
        parse_error(p, "truncated unicode escape");
    unsigned value = 0;
    for (int index = 0; index < 4; index++)
        value = value * 16 + hex_value(p, p->cursor[index]);
    p->cursor += 4;
    return value;
}

static void put_utf8(char **out, unsigned code)
{
    char *to = *out;
    if (code < 0x80) {
        *to++ = (char)code;
    } else if (code < 0x800) {
        *to++ = (char)(0xC0 | code >> 6);
        *to++ = (char)(0x80 | (code & 63));
    } else if (code < 0x10000) {
        *to++ = (char)(0xE0 | code >> 12);
        *to++ = (char)(0x80 | (code >> 6 & 63));
        *to++ = (char)(0x80 | (code & 63));
    } else {
        *to++ = (char)(0xF0 | code >> 18);
        *to++ = (char)(0x80 | (code >> 12 & 63));
        *to++ = (char)(0x80 | (code >> 6 & 63));
        *to++ = (char)(0x80 | (code & 63));
    }
    *out = to;
}

/* Rejects what Python's text decoding would have rejected before parsing. */
static void validate_utf8(parser *p, const char *from, const char *to)
{
    while (from < to) {
        unsigned char lead = (unsigned char)*from;
        if (lead < 0x80) {
            from++;
            continue;
        }
        int extra = lead >= 0xF0 ? 3 : lead >= 0xE0 ? 2 : lead >= 0xC2 ? 1 : -1;
        if (extra < 0 || lead > 0xF4 || to - from <= extra)
            parse_error(p, "invalid UTF-8");
        unsigned code = lead & (0x3F >> extra);
        for (int index = 1; index <= extra; index++) {
            unsigned char next = (unsigned char)from[index];
            if ((next & 0xC0) != 0x80)
                parse_error(p, "invalid UTF-8");
            code = code << 6 | (next & 63);
        }
        if ((extra == 2 && code < 0x800) || (extra == 3 && (code < 0x10000 || code > 0x10FFFF)) ||
            (code >= 0xD800 && code <= 0xDFFF))
            parse_error(p, "invalid UTF-8");
        from += extra + 1;
    }
}

static const char *parse_string(parser *p, size_t *length)
{
    /* Escapes never lengthen a string, so the raw span bounds the output. */
    const char *scan = p->cursor;
    while (scan < p->end && *scan != '"') {
        if (*scan == '\\')
            scan++;
        scan++;
    }
    if (scan >= p->end)
        parse_error(p, "unterminated string");
    validate_utf8(p, p->cursor, scan);
    char *copy = lhg_arena_alloc(p->arena, (size_t)(scan - p->cursor) + 1);
    char *out = copy;
    while (p->cursor < scan) {
        char c = *p->cursor++;
        if ((unsigned char)c < 0x20) {
            p->cursor--;
            parse_error(p, "control character in string");
        }
        if (c != '\\') {
            *out++ = c;
            continue;
        }
        c = *p->cursor++;
        switch (c) {
        case '"': *out++ = '"'; break;
        case '\\': *out++ = '\\'; break;
        case '/': *out++ = '/'; break;
        case 'b': *out++ = '\b'; break;
        case 'f': *out++ = '\f'; break;
        case 'n': *out++ = '\n'; break;
        case 'r': *out++ = '\r'; break;
        case 't': *out++ = '\t'; break;
        case 'u': {
            unsigned code = parse_u_escape(p);
            if (code >= 0xD800 && code < 0xDC00 && p->end - p->cursor >= 6 && p->cursor[0] == '\\' && p->cursor[1] == 'u') {
                const char *saved = p->cursor;
                p->cursor += 2;
                unsigned low = parse_u_escape(p);
                if (low >= 0xDC00 && low < 0xE000)
                    code = 0x10000 + ((code - 0xD800) << 10) + (low - 0xDC00);
                else
                    p->cursor = saved;
            }
            put_utf8(&out, code);
            break;
        }
        default:
            parse_error(p, "bad escape");
        }
    }
    p->cursor = scan + 1;
    *out = 0;
    *length = (size_t)(out - copy);
    return copy;
}

static lhg_json *new_value(parser *p, lhg_json_type type)
{
    lhg_json *value = lhg_arena_alloc(p->arena, sizeof *value);
    memset(value, 0, sizeof *value);
    value->type = type;
    return value;
}

static lhg_json *parse_value(parser *p);

static lhg_json *parse_number(parser *p)
{
    const char *start = p->cursor;
    int negative = 0;
    if (p->cursor < p->end && *p->cursor == '-') {
        negative = 1;
        p->cursor++;
    }
    if (p->cursor >= p->end || *p->cursor < '0' || *p->cursor > '9')
        parse_error(p, "bad number");
    if (*p->cursor == '0') {
        p->cursor++;
    } else {
        while (p->cursor < p->end && *p->cursor >= '0' && *p->cursor <= '9')
            p->cursor++;
    }
    int is_float = 0;
    if (p->cursor < p->end && *p->cursor == '.') {
        is_float = 1;
        p->cursor++;
        if (p->cursor >= p->end || *p->cursor < '0' || *p->cursor > '9')
            parse_error(p, "bad fraction");
        while (p->cursor < p->end && *p->cursor >= '0' && *p->cursor <= '9')
            p->cursor++;
    }
    if (p->cursor < p->end && (*p->cursor == 'e' || *p->cursor == 'E')) {
        is_float = 1;
        p->cursor++;
        if (p->cursor < p->end && (*p->cursor == '+' || *p->cursor == '-'))
            p->cursor++;
        if (p->cursor >= p->end || *p->cursor < '0' || *p->cursor > '9')
            parse_error(p, "bad exponent");
        while (p->cursor < p->end && *p->cursor >= '0' && *p->cursor <= '9')
            p->cursor++;
    }
    lhg_json *value = new_value(p, is_float ? LHG_JSON_FLOAT : LHG_JSON_INT);
    if (is_float) {
        value->as.number = strtod(start, NULL);
        return value;
    }
    uint64_t magnitude = 0;
    for (const char *at = start + negative; at < p->cursor; at++) {
        if (magnitude > (UINT64_C(9223372036854775807) - (uint64_t)(*at - '0')) / 10)
            parse_error(p, "integer out of range");
        magnitude = magnitude * 10 + (uint64_t)(*at - '0');
    }
    value->as.integer = negative ? -(int64_t)magnitude : (int64_t)magnitude;
    return value;
}

static void object_set(lhg_arena *arena, lhg_json *object, const char *key, size_t key_length, lhg_json *value)
{
    for (size_t index = 0; index < object->as.object.count; index++) {
        lhg_json_member *member = &object->as.object.members[index];
        if (member->key_length == key_length && !memcmp(member->key, key, key_length)) {
            member->value = value;
            return;
        }
    }
    if (object->as.object.count == object->as.object.capacity) {
        size_t next = object->as.object.capacity ? object->as.object.capacity * 2 : 8;
        lhg_json_member *grown = lhg_arena_alloc(arena, next * sizeof *grown);
        if (object->as.object.count)
            memcpy(grown, object->as.object.members, object->as.object.count * sizeof *grown);
        object->as.object.members = grown;
        object->as.object.capacity = next;
    }
    lhg_json_member *member = &object->as.object.members[object->as.object.count++];
    member->key = key;
    member->key_length = key_length;
    member->value = value;
}

static void array_push(lhg_arena *arena, lhg_json *array, lhg_json *value)
{
    if (array->as.array.count == array->as.array.capacity) {
        size_t next = array->as.array.capacity ? array->as.array.capacity * 2 : 8;
        lhg_json **grown = lhg_arena_alloc(arena, next * sizeof *grown);
        if (array->as.array.count)
            memcpy(grown, array->as.array.items, array->as.array.count * sizeof *grown);
        array->as.array.items = grown;
        array->as.array.capacity = next;
    }
    array->as.array.items[array->as.array.count++] = value;
}

static int match_word(parser *p, const char *word)
{
    size_t length = strlen(word);
    if ((size_t)(p->end - p->cursor) < length || memcmp(p->cursor, word, length))
        return 0;
    p->cursor += length;
    return 1;
}

static lhg_json *parse_value(parser *p)
{
    if (++p->depth > 256)
        parse_error(p, "nesting too deep");
    skip_space(p);
    if (p->cursor >= p->end)
        parse_error(p, "unexpected end of input");
    lhg_json *result;
    char c = *p->cursor;
    if (c == '{') {
        p->cursor++;
        result = new_value(p, LHG_JSON_OBJECT);
        skip_space(p);
        if (p->cursor < p->end && *p->cursor == '}') {
            p->cursor++;
        } else {
            for (;;) {
                skip_space(p);
                if (p->cursor >= p->end || *p->cursor != '"')
                    parse_error(p, "expected a key");
                p->cursor++;
                size_t key_length;
                const char *key = parse_string(p, &key_length);
                skip_space(p);
                if (p->cursor >= p->end || *p->cursor != ':')
                    parse_error(p, "expected ':'");
                p->cursor++;
                lhg_json *value = parse_value(p);
                object_set(p->arena, result, key, key_length, value);
                skip_space(p);
                if (p->cursor < p->end && *p->cursor == ',') {
                    p->cursor++;
                    continue;
                }
                if (p->cursor < p->end && *p->cursor == '}') {
                    p->cursor++;
                    break;
                }
                parse_error(p, "expected ',' or '}'");
            }
        }
    } else if (c == '[') {
        p->cursor++;
        result = new_value(p, LHG_JSON_ARRAY);
        skip_space(p);
        if (p->cursor < p->end && *p->cursor == ']') {
            p->cursor++;
        } else {
            for (;;) {
                array_push(p->arena, result, parse_value(p));
                skip_space(p);
                if (p->cursor < p->end && *p->cursor == ',') {
                    p->cursor++;
                    continue;
                }
                if (p->cursor < p->end && *p->cursor == ']') {
                    p->cursor++;
                    break;
                }
                parse_error(p, "expected ',' or ']'");
            }
        }
    } else if (c == '"') {
        p->cursor++;
        result = new_value(p, LHG_JSON_STRING);
        result->as.string.text = parse_string(p, &result->as.string.length);
    } else if (c == '-' || (c >= '0' && c <= '9')) {
        result = parse_number(p);
    } else if (match_word(p, "true")) {
        result = new_value(p, LHG_JSON_BOOL);
        result->as.boolean = 1;
    } else if (match_word(p, "false")) {
        result = new_value(p, LHG_JSON_BOOL);
    } else if (match_word(p, "null")) {
        result = new_value(p, LHG_JSON_NULL);
    } else {
        parse_error(p, "unexpected character");
    }
    p->depth--;
    return result;
}

lhg_json *lhg_json_parse(lhg_arena *arena, const char *data, size_t size, const char *name)
{
    parser p = {arena, arena->ctx, data, data, data + size, name, 0};
    lhg_json *value = parse_value(&p);
    skip_space(&p);
    if (p.cursor != p.end)
        parse_error(&p, "trailing data");
    return value;
}

const lhg_json *lhg_json_find(const lhg_json *object, const char *key)
{
    if (!object || object->type != LHG_JSON_OBJECT)
        return NULL;
    size_t length = strlen(key);
    for (size_t index = 0; index < object->as.object.count; index++) {
        const lhg_json_member *member = &object->as.object.members[index];
        if (member->key_length == length && !memcmp(member->key, key, length))
            return member->value;
    }
    return NULL;
}

const lhg_json *lhg_json_require(lhg_ctx *ctx, const lhg_json *object, const char *key, const char *where)
{
    const lhg_json *value = lhg_json_find(object, key);
    if (!value)
        lhg_fail(ctx, "%s: missing field '%s'", where, key);
    return value;
}

const char *lhg_json_str(lhg_ctx *ctx, const lhg_json *value, const char *where)
{
    if (!value || value->type != LHG_JSON_STRING)
        lhg_fail(ctx, "%s: expected a string", where);
    return value->as.string.text;
}

int64_t lhg_json_int(lhg_ctx *ctx, const lhg_json *value, const char *where)
{
    if (!value || value->type != LHG_JSON_INT)
        lhg_fail(ctx, "%s: expected an integer", where);
    return value->as.integer;
}

const lhg_json *lhg_json_list(lhg_ctx *ctx, const lhg_json *value, const char *where)
{
    if (!value || value->type != LHG_JSON_ARRAY)
        lhg_fail(ctx, "%s: expected a list", where);
    return value;
}

int lhg_json_truth(const lhg_json *value)
{
    if (!value)
        return 0;
    switch (value->type) {
    case LHG_JSON_NULL: return 0;
    case LHG_JSON_BOOL: return value->as.boolean;
    case LHG_JSON_INT: return value->as.integer != 0;
    case LHG_JSON_FLOAT: return value->as.number != 0;
    case LHG_JSON_STRING: return value->as.string.length != 0;
    case LHG_JSON_ARRAY: return value->as.array.count != 0;
    case LHG_JSON_OBJECT: return value->as.object.count != 0;
    }
    return 0;
}

static lhg_json *new_node(lhg_arena *arena, lhg_json_type type)
{
    lhg_json *value = lhg_arena_alloc(arena, sizeof *value);
    memset(value, 0, sizeof *value);
    value->type = type;
    return value;
}

lhg_json *lhg_json_new_null(lhg_arena *arena)
{
    return new_node(arena, LHG_JSON_NULL);
}

lhg_json *lhg_json_new_bool(lhg_arena *arena, int value)
{
    lhg_json *node = new_node(arena, LHG_JSON_BOOL);
    node->as.boolean = value != 0;
    return node;
}

lhg_json *lhg_json_new_int(lhg_arena *arena, int64_t value)
{
    lhg_json *node = new_node(arena, LHG_JSON_INT);
    node->as.integer = value;
    return node;
}

lhg_json *lhg_json_new_str(lhg_arena *arena, const char *text)
{
    lhg_json *node = new_node(arena, LHG_JSON_STRING);
    node->as.string.length = strlen(text);
    node->as.string.text = lhg_arena_strdup(arena, text, node->as.string.length);
    return node;
}

lhg_json *lhg_json_new_array(lhg_arena *arena)
{
    return new_node(arena, LHG_JSON_ARRAY);
}

lhg_json *lhg_json_new_object(lhg_arena *arena)
{
    return new_node(arena, LHG_JSON_OBJECT);
}

void lhg_json_append(lhg_arena *arena, lhg_json *array, lhg_json *value)
{
    array_push(arena, array, value);
}

void lhg_json_set(lhg_arena *arena, lhg_json *object, const char *key, lhg_json *value)
{
    object_set(arena, object, lhg_arena_strdup(arena, key, strlen(key)), strlen(key), value);
}

static unsigned next_code_point(const char **cursor, const char *end)
{
    const unsigned char *at = (const unsigned char *)*cursor;
    unsigned lead = *at;
    int extra = lead >= 0xF0 ? 3 : lead >= 0xE0 ? 2 : lead >= 0xC0 ? 1 : 0;
    unsigned code = extra ? lead & (0x3F >> extra) : lead;
    for (int index = 1; index <= extra && *cursor + index < end; index++)
        code = code << 6 | (at[index] & 63);
    *cursor += extra + 1;
    return code;
}

static void escape_string(lhg_text *out, const char *text, size_t length)
{
    static const char digits[] = "0123456789abcdef";
    lhg_text_add(out, "\"", 1);
    const char *cursor = text, *end = text + length;
    while (cursor < end) {
        unsigned code = next_code_point(&cursor, end);
        switch (code) {
        case '"': lhg_text_add(out, "\\\"", 2); continue;
        case '\\': lhg_text_add(out, "\\\\", 2); continue;
        case '\n': lhg_text_add(out, "\\n", 2); continue;
        case '\r': lhg_text_add(out, "\\r", 2); continue;
        case '\t': lhg_text_add(out, "\\t", 2); continue;
        case '\f': lhg_text_add(out, "\\f", 2); continue;
        case '\b': lhg_text_add(out, "\\b", 2); continue;
        default: break;
        }
        if (code >= 0x20 && code <= 0x7E) {
            char byte = (char)code;
            lhg_text_add(out, &byte, 1);
            continue;
        }
        unsigned units[2];
        int count = 1;
        units[0] = code;
        if (code >= 0x10000) {
            code -= 0x10000;
            units[0] = 0xD800 + (code >> 10);
            units[1] = 0xDC00 + (code & 1023);
            count = 2;
        }
        for (int index = 0; index < count; index++) {
            char escape[6] = {'\\', 'u', digits[units[index] >> 12 & 15], digits[units[index] >> 8 & 15],
                              digits[units[index] >> 4 & 15], digits[units[index] & 15]};
            lhg_text_add(out, escape, 6);
        }
    }
    lhg_text_add(out, "\"", 1);
}

static void add_spaces(lhg_text *out, int count)
{
    while (count-- > 0)
        lhg_text_add(out, " ", 1);
}

static void dump_value(lhg_text *out, const lhg_json *value, int indent)
{
    switch (value->type) {
    case LHG_JSON_NULL: lhg_text_adds(out, "null"); break;
    case LHG_JSON_BOOL: lhg_text_adds(out, value->as.boolean ? "true" : "false"); break;
    case LHG_JSON_INT: {
        char digits[24];
        int used = snprintf(digits, sizeof digits, "%lld", (long long)value->as.integer);
        lhg_text_add(out, digits, (size_t)used);
        break;
    }
    case LHG_JSON_FLOAT: {
        /* Nothing the generator writes is fractional; catalogs only pass integers through. */
        char digits[40];
        int used = snprintf(digits, sizeof digits, "%.17g", value->as.number);
        lhg_text_add(out, digits, (size_t)used);
        break;
    }
    case LHG_JSON_STRING: escape_string(out, value->as.string.text, value->as.string.length); break;
    case LHG_JSON_ARRAY: {
        if (!value->as.array.count) {
            lhg_text_adds(out, "[]");
            break;
        }
        lhg_text_adds(out, "[\n");
        for (size_t index = 0; index < value->as.array.count; index++) {
            add_spaces(out, indent + 2);
            dump_value(out, value->as.array.items[index], indent + 2);
            lhg_text_adds(out, index + 1 == value->as.array.count ? "\n" : ",\n");
        }
        add_spaces(out, indent);
        lhg_text_adds(out, "]");
        break;
    }
    case LHG_JSON_OBJECT: {
        if (!value->as.object.count) {
            lhg_text_adds(out, "{}");
            break;
        }
        lhg_text_adds(out, "{\n");
        for (size_t index = 0; index < value->as.object.count; index++) {
            const lhg_json_member *member = &value->as.object.members[index];
            add_spaces(out, indent + 2);
            escape_string(out, member->key, member->key_length);
            lhg_text_adds(out, ": ");
            dump_value(out, member->value, indent + 2);
            lhg_text_adds(out, index + 1 == value->as.object.count ? "\n" : ",\n");
        }
        add_spaces(out, indent);
        lhg_text_adds(out, "}");
        break;
    }
    }
}

void lhg_json_dump(lhg_text *out, const lhg_json *value)
{
    dump_value(out, value, 0);
}
