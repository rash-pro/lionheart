/* Function discovery. Results are plain data so any backend can consume them. */
#ifndef LHG_DISCOVER_H
#define LHG_DISCOVER_H

#include "base.h"
#include "decode.h"
#include "json.h"

/* Catalog-set facts for the manifest; only the full catalog has these. */
typedef struct {
    const char *name;
    const lhg_json *names, *paths;
    int discs[4];
    int disc_count;
    const char *catalog_sha1;
    int64_t catalog_size;
    int has_disc;
    int disc;
    int poll_forward;
} lhg_image_meta;

typedef struct {
    int64_t low, high;
} lhg_range;

/* Python computes addresses with unbounded integers; every address here stays
 * in 32 bits except the transient pc + 8 style sums, kept in int64_t. */
typedef struct {
    const uint8_t *data;
    size_t size;
    uint32_t base, entry;
    const lhg_range *code_ranges;
    size_t code_range_count;
    const uint32_t *entry_points;
    size_t entry_point_count;
    const uint32_t *timing_entry_points;
    size_t timing_entry_point_count;
    const lhg_image_meta *meta;
} lhg_image;

typedef struct {
    uint32_t pc;
    uint32_t count;
    uint32_t *targets;
} lhg_jump_table;

typedef struct {
    uint32_t entry;
    lhg_insn *insns;
    uint32_t insn_count;
    lhg_jump_table *tables;
    lhg_jump_table **tables_by_pc;
    uint32_t table_count;
    uint32_t *table_words;
    uint32_t table_word_count;
} lhg_function;

typedef struct {
    lhg_function *items;
    size_t count;
} lhg_functions;

int lhg_image_contains(const lhg_image *image, int64_t pc);
int lhg_image_executable(const lhg_image *image, int64_t pc);
uint32_t lhg_image_word(const lhg_image *image, int64_t pc);
lhg_insn lhg_image_insn(const lhg_image *image, int64_t pc);
void lhg_image_validate(lhg_ctx *ctx, const lhg_image *image);

/* Functions come back sorted by entry; instructions inside are sorted by pc.
 * Results live in the arena. */
void lhg_discover(lhg_arena *arena, const lhg_image *image, const uint32_t *seeds, size_t seed_count, int scan_calls,
                  lhg_functions *functions);
const lhg_function *lhg_functions_find(const lhg_functions *functions, uint32_t pc);
const lhg_insn *lhg_function_insn(const lhg_function *function, int64_t pc);
const lhg_jump_table *lhg_function_table(const lhg_function *function, int64_t pc);

/* Prologues and entry tables anchor traversal, never calls decoded from image pixels. */
uint32_t *lhg_prologue_seeds(lhg_arena *arena, const uint8_t *data, size_t size, uint32_t base, size_t *count);

#endif
