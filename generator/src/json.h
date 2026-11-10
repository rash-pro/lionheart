/* JSON read and write. Objects keep first-seen key order with last-seen value,
 * like the dictionaries the catalogs were written from. */
#ifndef LHG_JSON_H
#define LHG_JSON_H

#include "base.h"

typedef enum { LHG_JSON_NULL, LHG_JSON_BOOL, LHG_JSON_INT, LHG_JSON_FLOAT, LHG_JSON_STRING, LHG_JSON_ARRAY, LHG_JSON_OBJECT } lhg_json_type;

typedef struct lhg_json lhg_json;
typedef struct {
    const char *key;
    size_t key_length;
    lhg_json *value;
} lhg_json_member;

struct lhg_json {
    lhg_json_type type;
    union {
        int boolean;
        int64_t integer;
        double number;
        struct {
            const char *text;
            size_t length;
        } string;
        struct {
            lhg_json **items;
            size_t count, capacity;
        } array;
        struct {
            lhg_json_member *members;
            size_t count, capacity;
        } object;
    } as;
};

/* Strings are copied into the arena, so data may be released afterwards. */
lhg_json *lhg_json_parse(lhg_arena *arena, const char *data, size_t size, const char *name);

const lhg_json *lhg_json_find(const lhg_json *object, const char *key);
const lhg_json *lhg_json_require(lhg_ctx *ctx, const lhg_json *object, const char *key, const char *where);
const char *lhg_json_str(lhg_ctx *ctx, const lhg_json *value, const char *where);
int64_t lhg_json_int(lhg_ctx *ctx, const lhg_json *value, const char *where);
const lhg_json *lhg_json_list(lhg_ctx *ctx, const lhg_json *value, const char *where);
int lhg_json_truth(const lhg_json *value);

lhg_json *lhg_json_new_null(lhg_arena *arena);
lhg_json *lhg_json_new_bool(lhg_arena *arena, int value);
lhg_json *lhg_json_new_int(lhg_arena *arena, int64_t value);
lhg_json *lhg_json_new_str(lhg_arena *arena, const char *text);
lhg_json *lhg_json_new_array(lhg_arena *arena);
lhg_json *lhg_json_new_object(lhg_arena *arena);
void lhg_json_append(lhg_arena *arena, lhg_json *array, lhg_json *value);
/* Replaces in place when the key exists, otherwise appends. */
void lhg_json_set(lhg_arena *arena, lhg_json *object, const char *key, lhg_json *value);

/* Matches json.dumps(value, indent=2) in ASCII-escaped form. */
void lhg_json_dump(lhg_text *out, const lhg_json *value);

#endif
