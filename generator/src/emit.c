#include "emit.h"

#include <stdlib.h>
#include <string.h>

#include "locals.h"
#include "platform.h"
#include "sha1.h"

enum { UNIT_FUNCTIONS = 24 };

typedef struct {
    const char *prefix;
    const lhg_image *image;
    const lhg_functions *functions;
    int has_index, index;
    int poll_forward, direct_memory;
    uint32_t *owners;
    lhg_span_list *spans;
} emit_set;

typedef struct {
    uint64_t hash;
    uint32_t set, function;
    int used;
} owner_slot;

/* Interned text for per-unit symbol collection. */
typedef struct {
    lhg_arena *arena;
    const char **slots;
    size_t capacity, count;
} string_set;

static void string_set_init(string_set *set, lhg_arena *arena)
{
    set->arena = arena;
    set->capacity = 256;
    set->count = 0;
    set->slots = lhg_arena_alloc(arena, set->capacity * sizeof *set->slots);
    memset((void *)set->slots, 0, set->capacity * sizeof *set->slots);
}

static uint64_t hash_bytes(const char *text, size_t size)
{
    return lhg_fnv64_add(LHG_FNV64_INIT, (const uint8_t *)text, size);
}

static void string_set_grow(string_set *set)
{
    size_t capacity = set->capacity * 2;
    const char **slots = lhg_arena_alloc(set->arena, capacity * sizeof *slots);
    memset((void *)slots, 0, capacity * sizeof *slots);
    for (size_t index = 0; index < set->capacity; index++) {
        if (!set->slots[index])
            continue;
        size_t at = (size_t)hash_bytes(set->slots[index], strlen(set->slots[index])) & (capacity - 1);
        while (slots[at])
            at = (at + 1) & (capacity - 1);
        slots[at] = set->slots[index];
    }
    set->slots = slots;
    set->capacity = capacity;
}

static void string_set_add(string_set *set, const char *text, size_t size)
{
    if ((set->count + 1) * 2 > set->capacity)
        string_set_grow(set);
    size_t at = (size_t)hash_bytes(text, size) & (set->capacity - 1);
    while (set->slots[at]) {
        if (strlen(set->slots[at]) == size && !memcmp(set->slots[at], text, size))
            return;
        at = (at + 1) & (set->capacity - 1);
    }
    set->slots[at] = lhg_arena_strdup(set->arena, text, size);
    set->count++;
}

static int compare_strings(const void *a, const void *b)
{
    return strcmp(*(const char *const *)a, *(const char *const *)b);
}

static const char **string_set_sorted(string_set *set, lhg_arena *arena, size_t *count)
{
    const char **items = lhg_arena_alloc(arena, (set->count + 1) * sizeof *items);
    size_t used = 0;
    for (size_t index = 0; index < set->capacity; index++)
        if (set->slots[index])
            items[used++] = set->slots[index];
    qsort((void *)items, used, sizeof *items, compare_strings);
    *count = used;
    return items;
}

static int is_word_char(unsigned char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
}

static int is_hex8(const char *text)
{
    for (int index = 0; index < 8; index++) {
        char c = text[index];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')))
            return 0;
    }
    return 1;
}

static uint32_t parse_hex8(const char *text)
{
    uint32_t value = 0;
    for (int index = 0; index < 8; index++) {
        char c = text[index];
        value = value << 4 | (uint32_t)(c <= '9' ? c - '0' : c - 'a' + 10);
    }
    return value;
}

/* A maximal word that is (ov<digits>_)?<marker><8 hex>. Words are maximal by
 * construction, so \b at both ends of the original patterns needs no checking. */
static int matches_symbol(const char *word, size_t size, const char *marker)
{
    size_t marker_size = strlen(marker);
    size_t offset = 0;
    if (size > 2 && word[0] == 'o' && word[1] == 'v') {
        size_t at = 2;
        while (at < size && word[at] >= '0' && word[at] <= '9')
            at++;
        if (at > 2 && at < size && word[at] == '_')
            offset = at + 1;
    }
    if (offset + marker_size + 8 == size && !memcmp(word + offset, marker, marker_size) && is_hex8(word + offset + marker_size))
        return 1;
    return 0;
}

static const emit_set *owner_of(const emit_set *sets, const emit_set *set, const lhg_functions *functions, uint32_t pc)
{
    const lhg_function *found = lhg_functions_find(functions, pc);
    if (!found)
        return NULL;
    return &sets[set->owners[found - functions->items]];
}

typedef struct {
    string_set callees, regions;
    int uses_poll;
} unit_scan;

/* Rewrites one function body with its owning symbols and gathers the symbols
 * the unit must declare. A textual pass over whole words keeps this independent
 * of how the emitters happened to build their strings. */
static void finish_function(const lhg_text *raw, const emit_set *sets, const emit_set *set, const lhg_function *function,
                            const lhg_functions *main_functions, lhg_text *out, unit_scan *scan)
{
    char marker[16];
    int prefixed = set->prefix[0] != 0;
    lhg_arena *arena = scan->callees.arena;
    if (prefixed) {
        const char *text = lhg_fmt(arena, "code_%X", function->entry);
        memcpy(marker, text, 14);
    }
    const char *cursor = raw->data, *end = raw->data + raw->size;
    while (cursor < end) {
        if (!is_word_char((unsigned char)*cursor)) {
            const char *run = cursor;
            while (cursor < end && !is_word_char((unsigned char)*cursor))
                cursor++;
            lhg_text_add(out, run, (size_t)(cursor - run));
            continue;
        }
        const char *word = cursor;
        while (cursor < end && is_word_char((unsigned char)*cursor))
            cursor++;
        size_t size = (size_t)(cursor - word);
        char next = cursor < end ? *cursor : 0;
        size_t written_from = out->size;
        if (size == 10 && word[0] == 'f' && word[1] == '_' && is_hex8(word + 2)) {
            uint32_t pc = parse_hex8(word + 2);
            const emit_set *owner = owner_of(sets, set, set->functions, pc);
            if (!owner && set != sets)
                owner = owner_of(sets, sets, main_functions, pc);
            if (!owner)
                lhg_fail(out->ctx, "internal error: reference to an unknown function");
            lhg_text_add(out, owner->prefix, strlen(owner->prefix));
            lhg_text_add(out, word, size);
        } else if (prefixed && size >= 13) {
            /* str.replace semantics: every non-overlapping occurrence inside the word. */
            const char *search = word, *stop = word + size;
            while (search < stop) {
                const char *hit = NULL;
                for (const char *at = search; at + 13 <= stop; at++) {
                    if (!memcmp(at, marker, 13)) {
                        hit = at;
                        break;
                    }
                }
                if (!hit) {
                    lhg_text_add(out, search, (size_t)(stop - search));
                    break;
                }
                lhg_text_add(out, search, (size_t)(hit - search));
                lhg_text_add(out, set->prefix, strlen(set->prefix));
                lhg_text_add(out, marker, 13);
                search = hit + 13;
            }
        } else {
            lhg_text_add(out, word, size);
        }
        const char *final = out->data + written_from;
        size_t final_size = out->size - written_from;
        if (next == '(') {
            if (matches_symbol(final, final_size, "f_"))
                string_set_add(&scan->callees, final, final_size);
            if (final_size >= 11 && !memcmp(final + final_size - 11, "c_poll_skip", 11))
                scan->uses_poll = 1;
        }
        if (matches_symbol(final, final_size, "code_"))
            string_set_add(&scan->regions, final, final_size);
    }
}

static int compare_u32(const void *a, const void *b)
{
    uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;
    return x < y ? -1 : x > y;
}

static size_t count_unique_pcs(lhg_ctx *ctx, const lhg_function *const *functions, size_t count, uint32_t **sorted_out)
{
    size_t total = 0;
    for (size_t index = 0; index < count; index++)
        total += functions[index]->insn_count;
    uint32_t *pcs = lhg_alloc(ctx, (total + 1) * sizeof *pcs);
    size_t used = 0;
    for (size_t index = 0; index < count; index++)
        for (uint32_t row = 0; row < functions[index]->insn_count; row++)
            pcs[used++] = functions[index]->insns[row].pc;
    qsort(pcs, used, sizeof *pcs, compare_u32);
    size_t unique = 0;
    for (size_t index = 0; index < used; index++)
        if (!unique || pcs[unique - 1] != pcs[index])
            pcs[unique++] = pcs[index];
    if (sorted_out) {
        *sorted_out = pcs;
    } else {
        lhg_free(ctx, pcs);
    }
    return unique;
}

void lhg_code_spans(lhg_arena *arena, const lhg_function *f, lhg_span_list *list)
{
    lhg_span *spans = lhg_arena_alloc(arena, (f->insn_count + f->table_word_count + 1) * sizeof *spans);
    uint32_t count = 0, left = 0, right = 0;
    while (left < f->insn_count || right < f->table_word_count) {
        uint32_t pc;
        if (right >= f->table_word_count || (left < f->insn_count && f->insns[left].pc <= f->table_words[right])) {
            pc = f->insns[left].pc;
            if (right < f->table_word_count && f->table_words[right] == pc)
                right++;
            left++;
        } else {
            pc = f->table_words[right++];
        }
        if (count && (uint64_t)spans[count - 1].start + spans[count - 1].size == (uint64_t)pc)
            spans[count - 1].size += 4;
        else {
            spans[count].start = pc;
            spans[count].size = 4;
            count++;
        }
    }
    list->items = spans;
    list->count = count;
}

const uint8_t *lhg_image_bytes(const lhg_image *image, uint32_t pc)
{
    return image->data + (size_t)((int64_t)pc - (int64_t)image->base);
}

uint64_t lhg_spans_fingerprint(const lhg_image *image, const lhg_span_list *list)
{
    uint64_t state = LHG_FNV64_INIT;
    for (uint32_t index = 0; index < list->count; index++)
        state = lhg_fnv64_add(state, lhg_image_bytes(image, list->items[index].start), list->items[index].size);
    return state;
}

static int same_code(const emit_set *a, uint32_t a_function, const emit_set *b, uint32_t b_function)
{
    const lhg_function *fa = &a->functions->items[a_function], *fb = &b->functions->items[b_function];
    if (fa->entry != fb->entry)
        return 0;
    const lhg_span_list *la = &a->spans[a_function], *lb = &b->spans[b_function];
    if (la->count != lb->count || la->fingerprint != lb->fingerprint)
        return 0;
    for (uint32_t index = 0; index < la->count; index++) {
        if (la->items[index].start != lb->items[index].start || la->items[index].size != lb->items[index].size)
            return 0;
        if (memcmp(lhg_image_bytes(a->image, la->items[index].start), lhg_image_bytes(b->image, lb->items[index].start), la->items[index].size))
            return 0;
    }
    return 1;
}

/* The first set to contain identical code keeps its symbol; later sets borrow it. */
static void assign_owners(lhg_arena *arena, emit_set *sets, size_t set_count)
{
    lhg_ctx *ctx = arena->ctx;
    size_t total = 0;
    for (size_t index = 0; index < set_count; index++)
        total += sets[index].functions->count;
    size_t capacity = 64;
    while (capacity < total * 2 + 64)
        capacity *= 2;
    owner_slot *slots = lhg_zalloc(ctx, capacity, sizeof *slots);
    for (size_t set_index = 0; set_index < set_count; set_index++) {
        emit_set *set = &sets[set_index];
        size_t count = set->functions->count;
        set->owners = lhg_arena_alloc(arena, (count + 1) * sizeof *set->owners);
        set->spans = lhg_arena_alloc(arena, (count + 1) * sizeof *set->spans);
        for (uint32_t row = 0; row < count; row++) {
            const lhg_function *f = &set->functions->items[row];
            lhg_code_spans(arena, f, &set->spans[row]);
            set->spans[row].fingerprint = lhg_spans_fingerprint(set->image, &set->spans[row]);
            uint64_t hash = set->spans[row].fingerprint ^ ((uint64_t)f->entry * UINT64_C(0x9E3779B97F4A7C15));
            size_t at = (size_t)hash & (capacity - 1);
            uint32_t owner = (uint32_t)set_index;
            for (;;) {
                if (!slots[at].used) {
                    slots[at].used = 1;
                    slots[at].hash = hash;
                    slots[at].set = (uint32_t)set_index;
                    slots[at].function = row;
                    break;
                }
                if (slots[at].hash == hash && same_code(&sets[slots[at].set], slots[at].function, set, row)) {
                    owner = slots[at].set;
                    break;
                }
                at = (at + 1) & (capacity - 1);
            }
            set->owners[row] = owner;
        }
    }
    lhg_free(ctx, slots);
}

static int compare_entry_pair(const void *a, const void *b)
{
    const lhg_entry_pair *x = a, *y = b;
    if (x->pc != y->pc)
        return x->pc < y->pc ? -1 : 1;
    return x->sequence < y->sequence ? -1 : x->sequence > y->sequence;
}

/* Maps every resume point to the function that owns it. Function entries are
 * seeded first, so a shared leader keeps the earliest owner in entry order. */
size_t lhg_build_entry_map(lhg_ctx *ctx, const lhg_functions *functions, lhg_arena *scratch, int compact, lhg_entry_pair **out)
{
    size_t capacity = 16, count = 0;
    lhg_entry_pair *pairs = lhg_alloc(ctx, capacity * sizeof *pairs);
    for (size_t row = 0; row < functions->count; row++) {
        pairs = lhg_grow(ctx, pairs, &capacity, count, sizeof *pairs);
        pairs[count].pc = functions->items[row].entry;
        pairs[count].owner = functions->items[row].entry;
        pairs[count].sequence = (uint32_t)count;
        count++;
    }
    for (size_t row = 0; row < functions->count; row++) {
        const lhg_function *f = &functions->items[row];
        lhg_analysis analysis;
        lhg_analyze(scratch, f, &analysis);
        const uint8_t *flags = compact ? analysis.compact_entries : analysis.full_entries;
        for (uint32_t index = 0; index < f->insn_count; index++) {
            if (!flags[index])
                continue;
            pairs = lhg_grow(ctx, pairs, &capacity, count, sizeof *pairs);
            pairs[count].pc = f->insns[index].pc;
            pairs[count].owner = f->entry;
            pairs[count].sequence = (uint32_t)count;
            count++;
        }
        lhg_arena_reset(scratch);
    }
    qsort(pairs, count, sizeof *pairs, compare_entry_pair);
    size_t unique = 0;
    for (size_t index = 0; index < count; index++)
        if (!unique || pairs[unique - 1].pc != pairs[index].pc)
            pairs[unique++] = pairs[index];
    *out = pairs;
    return unique;
}

static int same_entry_maps(const lhg_entry_pair *a, size_t a_count, const lhg_entry_pair *b, size_t b_count)
{
    if (a_count != b_count)
        return 0;
    for (size_t index = 0; index < a_count; index++)
        if (a[index].pc != b[index].pc || a[index].owner != b[index].owner)
            return 0;
    return 1;
}

static void add_line(lhg_text *text, const char *line)
{
    lhg_text_adds(text, line);
    lhg_text_add(text, "\n", 1);
}

static void emit_map_entries(lhg_text *tables, lhg_arena *scratch, const lhg_entry_pair *pairs, size_t count, const emit_set *sets,
                             const emit_set *set, const lhg_functions *main_functions)
{
    if (!count) {
        add_line(tables, "{0, NULL},");
        return;
    }
    for (size_t index = 0; index < count; index++) {
        const emit_set *owner = owner_of(sets, set, set->functions, pairs[index].owner);
        if (!owner)
            owner = owner_of(sets, sets, main_functions, pairs[index].owner);
        lhg_text_addf(tables, scratch, "{%U, %sf_%X},\n", pairs[index].pc, owner->prefix, pairs[index].owner);
    }
    lhg_arena_reset(scratch);
}

static void emit_map_regions(lhg_text *tables, lhg_arena *scratch, const lhg_entry_pair *pairs, size_t count, const emit_set *sets,
                             const emit_set *set)
{
    if (!count) {
        add_line(tables, "NULL,");
        return;
    }
    for (size_t index = 0; index < count; index++) {
        const emit_set *owner = owner_of(sets, set, set->functions, pairs[index].owner);
        lhg_text_addf(tables, scratch, "&%scode_%X,\n", owner->prefix, pairs[index].owner);
    }
    lhg_arena_reset(scratch);
}

typedef struct {
    const char **names;
    size_t count, capacity;
} name_list;

typedef struct {
    const char *output;
    lhg_arena *arena;
    const name_list *sources;
} stale_scan;

static int name_matches(const char *name, const char *prefix)
{
    size_t name_size = strlen(name), prefix_size = strlen(prefix);
    return name_size >= prefix_size + 2 && !memcmp(name, prefix, prefix_size) && !strcmp(name + name_size - 2, ".c");
}

static void remove_stale(lhg_ctx *ctx, void *user, const char *name)
{
    stale_scan *scan = user;
    if (!name_matches(name, "functions_") && !name_matches(name, "overlay_"))
        return;
    for (size_t index = 0; index < scan->sources->count; index++)
        if (!strcmp(scan->sources->names[index], name))
            return;
    lhg_remove_file(ctx, lhg_join_path(scan->arena, scan->output, name));
}

static void emit_function_text(lhg_arena *arena, const lhg_function *f, const lhg_available *available, const lhg_emit_options *options,
                               int poll_forward, int direct_memory, int hot, int fused, lhg_text *raw)
{
    lhg_analysis analysis;
    lhg_analyze(arena, f, &analysis);
    lhg_text_clear(raw);
    if (options->timing != LHG_TIMING_EXACT) {
        lhg_emit_timing(arena, &analysis, available, options->timing, hot || f->insn_count <= 32, fused, raw);
        if (hot && lhg_cache_qualify(f)) {
            lhg_text cached;
            lhg_text_init(&cached, raw->ctx);
            lhg_text_adds(&cached, "#ifdef FF8_PLAY_REG_CACHE\n");
            lhg_cache_lower(arena, raw, options->register_cache->max_registers, &cached);
            lhg_text_adds(&cached, "#else\n");
            lhg_text_add(&cached, raw->data, raw->size);
            lhg_text_adds(&cached, "#endif\n");
            lhg_text_clear(raw);
            lhg_text_add(raw, cached.data, cached.size);
            lhg_text_free(&cached);
        }
        return;
    }
    int trace = options->mode == LHG_MODE_TRACE;
    if (trace) {
        lhg_emit_fast_variant(arena, &analysis, available, 1, 0, 0, 0, raw);
        return;
    }
    lhg_text_adds(raw, "#if defined(FF8_COMPACT)\n");
    if (direct_memory) {
        lhg_text_adds(raw, "#if defined(FF8_COMPACT) || defined(FF8_DIRECT_RAM)\n");
        lhg_emit_fast_variant(arena, &analysis, available, 0, 1, poll_forward, 1, raw);
        lhg_text_adds(raw, "#else\n");
        lhg_emit_fast_variant(arena, &analysis, available, 0, 1, poll_forward, 0, raw);
        lhg_text_adds(raw, "#endif\n");
    } else {
        lhg_emit_fast_variant(arena, &analysis, available, 0, 1, poll_forward, 0, raw);
    }
    lhg_text_adds(raw, "#else\n");
    if (options->host_mode == LHG_HOST_PRECISE)
        lhg_emit_precise(arena, &analysis, raw);
    else
        lhg_emit_fast_variant(arena, &analysis, available, 0, 0, 0, 0, raw);
    lhg_text_adds(raw, "#endif\n");
}

void lhg_emit(lhg_arena *arena, const lhg_image *image, const lhg_functions *functions, const char *output,
              const lhg_overlay_set *overlays, size_t overlay_count, const lhg_emit_options *options,
              lhg_emit_result *result)
{
    lhg_ctx *ctx = arena->ctx;
    if (options->timing != LHG_TIMING_EXACT && options->mode == LHG_MODE_TRACE)
        lhg_fail(ctx, "experimental timing requires fast emission");
    lhg_make_directories(ctx, output);

    size_t set_count = overlay_count + 1;
    emit_set *sets = lhg_zalloc(ctx, set_count, sizeof *sets);
    sets[0].prefix = "";
    sets[0].image = image;
    sets[0].functions = functions;
    sets[0].poll_forward = 1;
    sets[0].direct_memory = 1;
    for (size_t index = 0; index < overlay_count; index++) {
        emit_set *set = &sets[index + 1];
        set->prefix = lhg_fmt(arena, "ov%d_", overlays[index].index);
        set->image = overlays[index].image;
        set->functions = &overlays[index].functions;
        set->has_index = 1;
        set->index = overlays[index].index;
        const lhg_image_meta *meta = overlays[index].image->meta;
        int top_forward = set->index % 32 == 0 || set->index % 32 == 1;
        set->poll_forward = meta ? meta->poll_forward : top_forward;
        set->direct_memory = set->poll_forward || (meta && !strcmp(meta->name, "movie")) || (!meta && set->index % 32 == 27);
    }
    assign_owners(arena, sets, set_count);

    lhg_arena unit_arena, function_arena, json_arena;
    lhg_arena_init(&unit_arena, ctx);
    lhg_arena_init(&function_arena, ctx);
    lhg_arena_init(&json_arena, ctx);
    lhg_text raw, body, unit, declarations, tables, profiles;
    lhg_text_init(&raw, ctx);
    lhg_text_init(&body, ctx);
    lhg_text_init(&unit, ctx);
    lhg_text_init(&declarations, ctx);
    lhg_text_init(&tables, ctx);
    lhg_text_init(&profiles, ctx);

    name_list sources = {0};
    lhg_json *manifest_sets = lhg_json_new_array(&json_arena);
    lhg_json *units_json = lhg_json_new_array(&json_arena);
    size_t main_instructions = 0, main_entries = 0, main_compact_entries = 0;
    unsigned profile_lines = 0;
    string_set used_forward;
    string_set_init(&used_forward, &json_arena);

    for (size_t set_index = 0; set_index < set_count; set_index++) {
        emit_set *set = &sets[set_index];
        lhg_available available = {set->functions, set->has_index ? functions : NULL};
        const lhg_function **items = lhg_alloc(ctx, (set->functions->count + 1) * sizeof *items);
        const lhg_function **all_items = lhg_alloc(ctx, (set->functions->count + 1) * sizeof *all_items);
        size_t item_count = 0;
        for (size_t row = 0; row < set->functions->count; row++) {
            all_items[row] = &set->functions->items[row];
            if (&sets[set->owners[row]] == set)
                items[item_count++] = &set->functions->items[row];
        }
        for (size_t row = 0; row < item_count; row++) {
            uint32_t pc = items[row]->entry;
            lhg_text_addf(&declarations, &unit_arena, "void %sf_%X(cpu_t *c);\n", set->prefix, pc);
            lhg_text_addf(&declarations, &unit_arena, "extern const code_region_t %scode_%X;\n", set->prefix, pc);
        }
        for (size_t start = 0; start < item_count; start += UNIT_FUNCTIONS) {
            char digits[16];
            snprintf(digits, sizeof digits, "%03u", (unsigned)(start / UNIT_FUNCTIONS));
            const char *name = set->has_index ? lhg_fmt(arena, "overlay_%d_%s.c", set->index, digits) : lhg_fmt(arena, "functions_%s.c", digits);
            sources.names = lhg_grow(ctx, (void *)sources.names, &sources.capacity, sources.count, sizeof *sources.names);
            sources.names[sources.count++] = name;
            size_t stop = start + UNIT_FUNCTIONS < item_count ? start + UNIT_FUNCTIONS : item_count;
            lhg_text_clear(&body);
            unit_scan scan;
            string_set_init(&scan.callees, &unit_arena);
            string_set_init(&scan.regions, &unit_arena);
            scan.uses_poll = 0;
            for (size_t row = start; row < stop; row++) {
                const lhg_register_cache *selection = options->register_cache;
                const char *symbol = lhg_fmt(&function_arena, "%sf_%X", set->prefix, items[row]->entry);
                int hot = selection && lhg_cache_contains(selection->functions, selection->function_count, symbol);
                int fused = selection && lhg_cache_contains(selection->fuse_forward, selection->fuse_forward_count, symbol);
                if (fused && options->timing != LHG_TIMING_EXACT)
                    string_set_add(&used_forward, symbol, strlen(symbol));
                emit_function_text(&function_arena, items[row], &available, options, set->poll_forward, set->direct_memory, hot, fused, &raw);
                if (row != start)
                    lhg_text_add(&body, "\n", 1);
                finish_function(&raw, sets, set, items[row], functions, &body, &scan);
                lhg_arena_reset(&function_arena);
            }
            size_t callee_count, region_count;
            const char **callees = string_set_sorted(&scan.callees, &unit_arena, &callee_count);
            const char **regions = string_set_sorted(&scan.regions, &unit_arena, &region_count);
            lhg_text_clear(&unit);
            lhg_text_adds(&unit, "#include \"generated.h\"\n");
            if (scan.uses_poll)
                lhg_text_adds(&unit, "#include \"poll.h\"\n");
            for (size_t row = 0; row < callee_count; row++) {
                lhg_text_addf(&unit, &unit_arena, "void %s(cpu_t *c);", callees[row]);
                if (row + 1 < callee_count || region_count)
                    lhg_text_adds(&unit, "\n");
            }
            for (size_t row = 0; row < region_count; row++) {
                lhg_text_addf(&unit, &unit_arena, "extern const code_region_t %s;", regions[row]);
                if (row + 1 < region_count)
                    lhg_text_adds(&unit, "\n");
            }
            lhg_text_adds(&unit, "\n");
            lhg_text_add(&unit, body.data, body.size);
            lhg_write_if_changed(ctx, lhg_join_path(arena, output, name), unit.data, unit.size);
            lhg_arena_reset(&unit_arena);

            lhg_json *row_json = lhg_json_new_object(&json_arena);
            lhg_json_set(&json_arena, row_json, "source", lhg_json_new_str(&json_arena, name));
            lhg_json_set(&json_arena, row_json, "set", set->has_index ? lhg_json_new_int(&json_arena, set->index) : lhg_json_new_null(&json_arena));
            lhg_json_set(&json_arena, row_json, "functions", lhg_json_new_int(&json_arena, (int64_t)(stop - start)));
            lhg_json_set(&json_arena, row_json, "instructions", lhg_json_new_int(&json_arena, (int64_t)count_unique_pcs(ctx, items + start, stop - start, NULL)));
            lhg_json_append(&json_arena, units_json, row_json);
        }

        for (size_t row = 0; row < item_count; row++) {
            const lhg_function *f = items[row];
            size_t function_row = (size_t)(f - set->functions->items);
            const lhg_span_list *spans = &set->spans[function_row];
            const char *name = lhg_fmt(&unit_arena, "%scode_%X", set->prefix, f->entry);
            lhg_text_addf(&tables, &unit_arena, "static const code_span_t %s_spans[] = {", name);
            for (uint32_t s = 0; s < spans->count; s++) {
                if (s)
                    lhg_text_add(&tables, ",", 1);
                lhg_text_addf(&tables, &unit_arena, "{%U, %d}", spans->items[s].start, (int)spans->items[s].size);
            }
            lhg_text_adds(&tables, "};\n");
            lhg_analysis analysis;
            lhg_analyze(&function_arena, f, &analysis);
            lhg_text_addf(&tables, &unit_arena, "static const uint8_t %s_windows[] = {", name);
            int any = 0;
            for (uint32_t s = 0; s < spans->count; s++) {
                for (uint32_t offset = 0; offset < spans->items[s].size; offset += 4) {
                    const lhg_insn *insn = lhg_function_insn(f, (int64_t)spans->items[s].start + offset);
                    int value = insn ? analysis.distance[insn - f->insns] : 0;
                    if (value > 255)
                        value = 255;
                    char digits[8];
                    int used = snprintf(digits, sizeof digits, "%d", value);
                    if (any)
                        lhg_text_add(&tables, ",", 1);
                    any = 1;
                    lhg_text_add(&tables, digits, (size_t)used);
                }
            }
            lhg_text_adds(&tables, "};\n");
            lhg_text_addf(&tables, &unit_arena, "const code_region_t %s = {%s_spans, %d, UINT64_C(%Q), %d, %s_windows};\n", name, name,
                          (int)spans->count, spans->fingerprint, (int)analysis.event_gap, name);
            lhg_arena_reset(&function_arena);
            lhg_arena_reset(&unit_arena);
        }
        const char *table_name = set->has_index ? lhg_fmt(arena, "%sentries", set->prefix) : "generated_entries";
        const char *region_name = set->has_index ? lhg_fmt(arena, "%sregions", set->prefix) : "generated_regions";
        lhg_text_addf(&tables, &unit_arena, "const entry_t %s[] = {\n", table_name);
        lhg_entry_pair *dispatch_pairs, *compact_pairs;
        size_t dispatch_count = lhg_build_entry_map(ctx, set->functions, &function_arena, 0, &dispatch_pairs);
        size_t compact_count = lhg_build_entry_map(ctx, set->functions, &function_arena, 1, &compact_pairs);
        int same_maps = same_entry_maps(dispatch_pairs, dispatch_count, compact_pairs, compact_count);
        if (!same_maps) {
            add_line(&tables, "#if defined(FF8_COMPACT)");
            emit_map_entries(&tables, &unit_arena, compact_pairs, compact_count, sets, set, functions);
            add_line(&tables, "#else");
        }
        emit_map_entries(&tables, &unit_arena, dispatch_pairs, dispatch_count, sets, set, functions);
        if (!same_maps)
            add_line(&tables, "#endif");
        add_line(&tables, "};");
        lhg_text_addf(&tables, &unit_arena, "const code_region_t *const %s[] = {\n", region_name);
        if (!same_maps) {
            add_line(&tables, "#if defined(FF8_COMPACT)");
            emit_map_regions(&tables, &unit_arena, compact_pairs, compact_count, sets, set);
            add_line(&tables, "#else");
        }
        emit_map_regions(&tables, &unit_arena, dispatch_pairs, dispatch_count, sets, set);
        if (!same_maps)
            add_line(&tables, "#endif");
        add_line(&tables, "};");
        lhg_arena_reset(&unit_arena);

        size_t all_instructions = count_unique_pcs(ctx, all_items, set->functions->count, NULL);
        size_t emitted_instructions = count_unique_pcs(ctx, items, item_count, NULL);
        if (set_index == 0) {
            main_instructions = all_instructions;
            main_entries = dispatch_count;
            main_compact_entries = compact_count;
        } else {
            char sha1[41];
            lhg_sha1_hex(set->image->data, set->image->size, sha1);
            lhg_json *row = lhg_json_new_object(&json_arena);
            lhg_json_set(&json_arena, row, "index", lhg_json_new_int(&json_arena, set->index));
            lhg_json_set(&json_arena, row, "base", lhg_json_new_str(&json_arena, lhg_fmt(&json_arena, "%H", (uint64_t)set->image->base)));
            lhg_json_set(&json_arena, row, "size", lhg_json_new_int(&json_arena, (int64_t)set->image->size));
            lhg_json_set(&json_arena, row, "sha1", lhg_json_new_str(&json_arena, sha1));
            lhg_json_set(&json_arena, row, "functions", lhg_json_new_int(&json_arena, (int64_t)set->functions->count));
            lhg_json_set(&json_arena, row, "instructions", lhg_json_new_int(&json_arena, (int64_t)all_instructions));
            lhg_json_set(&json_arena, row, "emitted_functions", lhg_json_new_int(&json_arena, (int64_t)item_count));
            lhg_json_set(&json_arena, row, "emitted_instructions", lhg_json_new_int(&json_arena, (int64_t)emitted_instructions));
            /* Python floor-divides; indices are never negative here. */
            lhg_json_set(&json_arena, row, "disc", lhg_json_new_int(&json_arena, set->index / 32 + 1));
            lhg_json *dependencies = lhg_json_new_array(&json_arena);
            string_set dep_set;
            string_set_init(&dep_set, &unit_arena);
            for (size_t f_row = 0; f_row < set->functions->count; f_row++)
                if (&sets[set->owners[f_row]] != set)
                    string_set_add(&dep_set, sets[set->owners[f_row]].prefix, strlen(sets[set->owners[f_row]].prefix));
            size_t dep_count;
            const char **deps = string_set_sorted(&dep_set, &unit_arena, &dep_count);
            for (size_t dep = 0; dep < dep_count; dep++)
                lhg_json_append(&json_arena, dependencies, lhg_json_new_str(&json_arena, deps[dep]));
            lhg_arena_reset(&unit_arena);
            lhg_json_set(&json_arena, row, "dependencies", dependencies);
            lhg_json_set(&json_arena, row, "dispatch_entries", lhg_json_new_int(&json_arena, (int64_t)dispatch_count));
            lhg_json_set(&json_arena, row, "compact_dispatch_entries", lhg_json_new_int(&json_arena, (int64_t)compact_count));
            const lhg_image_meta *meta = set->image->meta;
            if (meta) {
                lhg_json_set(&json_arena, row, "name", lhg_json_new_str(&json_arena, meta->name));
                lhg_json_set(&json_arena, row, "names", (lhg_json *)meta->names);
                lhg_json *discs = lhg_json_new_array(&json_arena);
                for (int disc = 0; disc < meta->disc_count; disc++)
                    lhg_json_append(&json_arena, discs, lhg_json_new_int(&json_arena, meta->discs[disc]));
                lhg_json_set(&json_arena, row, "discs", discs);
                lhg_json_set(&json_arena, row, "paths", (lhg_json *)meta->paths);
                lhg_json_set(&json_arena, row, "catalog_sha1", lhg_json_new_str(&json_arena, meta->catalog_sha1));
                lhg_json_set(&json_arena, row, "catalog_size", lhg_json_new_int(&json_arena, meta->catalog_size));
                /* The metadata value replaces the earlier disc field in place. */
                lhg_json_set(&json_arena, row, "disc", meta->has_disc ? lhg_json_new_int(&json_arena, meta->disc) : lhg_json_new_null(&json_arena));
                lhg_json_set(&json_arena, row, "poll_forward", lhg_json_new_bool(&json_arena, meta->poll_forward));
            }
            lhg_json_append(&json_arena, manifest_sets, row);

            uint32_t *words = NULL;
            size_t word_count = count_unique_pcs(ctx, all_items, set->functions->count, &words);
            size_t sample_count = word_count < 16 ? word_count : 16;
            if (profile_lines++)
                lhg_text_adds(&profiles, "\n");
            lhg_text_addf(&profiles, &unit_arena, "%d %X %D %d ", set->index, set->image->base, (int64_t)set->image->size, (int)sample_count);
            int first_sample = 1;
            for (size_t sample = 0; sample < sample_count; sample++) {
                size_t position = word_count > 1 ? sample * (word_count - 1) / (word_count - 1 < 15 ? word_count - 1 : 15) : sample;
                if (!first_sample)
                    lhg_text_adds(&profiles, " ");
                first_sample = 0;
                lhg_text_addf(&profiles, &unit_arena, "%X %X", words[position], lhg_image_word(set->image, (int64_t)words[position]));
            }
            lhg_arena_reset(&unit_arena);
            lhg_free(ctx, words);
        }
        lhg_free(ctx, dispatch_pairs);
        lhg_free(ctx, compact_pairs);
        lhg_free(ctx, items);
        lhg_free(ctx, all_items);
        lhg_progress(ctx, "emit", (unsigned long)(set_index + 1), (unsigned long)set_count);
    }

    add_line(&tables, "const overlay_t generated_overlays[] = {");
    for (size_t index = 1; index < set_count; index++) {
        const emit_set *set = &sets[index];
        uint8_t digest[20];
        lhg_sha1(set->image->data, set->image->size, digest);
        lhg_text_addf(&tables, &unit_arena, "{%d, %U, %D, {", set->index, set->image->base, (int64_t)set->image->size);
        for (int byte = 0; byte < 20; byte++) {
            if (byte)
                lhg_text_add(&tables, ",", 1);
            lhg_text_addf(&tables, &unit_arena, "%d", (int)digest[byte]);
        }
        lhg_text_addf(&tables, &unit_arena, "}, %sentries, sizeof(%sentries)/sizeof(entry_t), %sregions},\n", set->prefix, set->prefix, set->prefix);
    }
    if (set_count == 1)
        add_line(&tables, "{0, 0, 0, {0}, NULL, 0, NULL},");
    add_line(&tables, "};");
    lhg_text_addf(&tables, &unit_arena, "const size_t generated_overlay_count = %D;\n", (int64_t)overlay_count);

    lhg_text generated;
    lhg_text_init(&generated, ctx);
    lhg_text_adds(&generated,
                  "#ifndef FF8_GENERATED_H\n#define FF8_GENERATED_H\n#define GENERATED_HAS_OVERLAYS 1\n");
    if (options->timing != LHG_TIMING_EXACT)
        lhg_text_adds(&generated, "#define FF8_COMPACT 1\n#include \"timing.h\"\n");
    lhg_text_adds(&generated, "#include \"cpu.h\"\n#include \"gte.h\"\n"
                  "\nextern const entry_t generated_entries[];\nextern const size_t generated_count;\n"
                  "extern const code_region_t *const generated_regions[];\nextern const overlay_t generated_overlays[];\nextern const size_t generated_overlay_count;\n"
                  "extern const uint32_t generated_hash, generated_base, generated_size;\n#endif\n");
    lhg_write_if_changed(ctx, lhg_join_path(arena, output, "generated.h"), generated.data, generated.size);
    lhg_text_free(&generated);

    uint32_t hash32 = lhg_fnv1a(image->data, image->size);
    lhg_text dispatch;
    lhg_text_init(&dispatch, ctx);
    lhg_text_adds(&dispatch, "#include \"generated.h\"\n");
    lhg_text_add(&dispatch, declarations.data, declarations.size);
    lhg_text_add(&dispatch, tables.data, tables.size);
    /* Both lists were written newline-terminated; the generated file joins
     * them with no trailing newline before the footer starts. */
    if (dispatch.size && dispatch.data[dispatch.size - 1] == '\n')
        dispatch.size--;
    lhg_text_addf(&dispatch, &unit_arena, "\n#if defined(FF8_COMPACT)\nconst size_t generated_count = %D;\n#else\n", (int64_t)main_compact_entries);
    lhg_text_addf(&dispatch, &unit_arena, "const size_t generated_count = %D;\n#endif\n", (int64_t)main_entries);
    lhg_text_addf(&dispatch, &unit_arena, "const uint32_t generated_hash = %U;\n", hash32);
    lhg_text_addf(&dispatch, &unit_arena, "const uint32_t generated_base = %U, generated_size = %D;\n", image->base, (int64_t)image->size);
    lhg_text_addf(&dispatch, &unit_arena, "const unsigned generated_host_precise = %d;\n",
                  options->host_mode == LHG_HOST_PRECISE && options->mode != LHG_MODE_TRACE && options->timing == LHG_TIMING_EXACT);
    if (options->timing != LHG_TIMING_EXACT)
        lhg_text_addf(&dispatch, &unit_arena, "const uint32_t generated_timing_event_mask = %d;\n", options->timing == LHG_TIMING_LOOSE ? 255 : 0);
    lhg_write_if_changed(ctx, lhg_join_path(arena, output, "dispatch_table.c"), dispatch.data, dispatch.size);
    lhg_text_free(&dispatch);

    lhg_json *manifest = lhg_json_new_object(&json_arena);
    lhg_json_set(&json_arena, manifest, "mode", lhg_json_new_str(&json_arena, options->mode == LHG_MODE_TRACE ? "trace" : "fast"));
    lhg_json_set(&json_arena, manifest, "host_mode", lhg_json_new_str(&json_arena, options->host_mode == LHG_HOST_PRECISE ? "precise" : "fast"));
    lhg_json_set(&json_arena, manifest, "entry", lhg_json_new_str(&json_arena, lhg_fmt(&json_arena, "%H", (uint64_t)image->entry)));
    lhg_json_set(&json_arena, manifest, "base", lhg_json_new_str(&json_arena, lhg_fmt(&json_arena, "%H", (uint64_t)image->base)));
    lhg_json_set(&json_arena, manifest, "size", lhg_json_new_int(&json_arena, (int64_t)image->size));
    lhg_json_set(&json_arena, manifest, "fnv1a", lhg_json_new_str(&json_arena, lhg_fmt(&json_arena, "%H", (uint64_t)hash32)));
    lhg_json_set(&json_arena, manifest, "functions", lhg_json_new_int(&json_arena, (int64_t)functions->count));
    lhg_json_set(&json_arena, manifest, "instructions", lhg_json_new_int(&json_arena, (int64_t)main_instructions));
    lhg_json_set(&json_arena, manifest, "overlays", manifest_sets);
    lhg_json_set(&json_arena, manifest, "translation_units", units_json);
    lhg_json *jump_tables = lhg_json_new_object(&json_arena);
    size_t jump_table_count = 0;
    uint8_t *seen_table = lhg_zalloc(ctx, image->size / 4 + 2, 1);
    for (size_t row = 0; row < functions->count; row++) {
        const lhg_function *f = &functions->items[row];
        for (uint32_t table_index = 0; table_index < f->table_count; table_index++) {
            const lhg_jump_table *table = &f->tables[table_index];
            size_t word_index = (size_t)((int64_t)table->pc - (int64_t)(image->base & ~3u)) / 4;
            if (seen_table[word_index])
                continue;
            seen_table[word_index] = 1;
            lhg_json *targets = lhg_json_new_array(&json_arena);
            for (uint32_t target = 0; target < table->count; target++)
                lhg_json_append(&json_arena, targets, lhg_json_new_str(&json_arena, lhg_fmt(&json_arena, "%H", (uint64_t)table->targets[target])));
            lhg_json_set(&json_arena, jump_tables, lhg_fmt(&json_arena, "%H", (uint64_t)table->pc), targets);
            jump_table_count++;
        }
    }
    lhg_free(ctx, seen_table);
    lhg_json_set(&json_arena, manifest, "jump_tables", jump_tables);
    if (options->timing != LHG_TIMING_EXACT) {
        lhg_json_set(&json_arena, manifest, "timing", lhg_json_new_str(&json_arena, options->timing == LHG_TIMING_LOOSE ? "loose" : "block"));
        lhg_json *entries = lhg_json_new_array(&json_arena);
        for (size_t index = 0; index < image->timing_entry_point_count; index++)
            if (lhg_functions_find(functions, image->timing_entry_points[index]))
                lhg_json_append(&json_arena, entries, lhg_json_new_str(&json_arena, lhg_fmt(&json_arena, "%H", (uint64_t)image->timing_entry_points[index])));
        lhg_json_set(&json_arena, manifest, "timing_entry_points", entries);
        if (used_forward.count) {
            size_t count;
            const char **names = string_set_sorted(&used_forward, &json_arena, &count);
            lhg_json *regions = lhg_json_new_array(&json_arena);
            for (size_t index = 0; index < count; index++)
                lhg_json_append(&json_arena, regions, lhg_json_new_str(&json_arena, names[index]));
            lhg_json_set(&json_arena, manifest, "forward_regions", regions);
        }
    }
    lhg_text manifest_text;
    lhg_text_init(&manifest_text, ctx);
    lhg_json_dump(&manifest_text, manifest);
    lhg_text_adds(&manifest_text, "\n");
    lhg_write_if_changed(ctx, lhg_join_path(arena, output, "manifest.json"), manifest_text.data, manifest_text.size);
    lhg_text_free(&manifest_text);

    lhg_text_add(&profiles, "\n", 1);
    lhg_write_if_changed(ctx, lhg_join_path(arena, output, "profile-catalog.txt"), profiles.data, profiles.size);

    lhg_text cmake;
    lhg_text_init(&cmake, ctx);
    lhg_text_adds(&cmake, "set(GENERATED_SOURCES\n");
    for (size_t row = 0; row < sources.count; row++) {
        lhg_text_addf(&cmake, &unit_arena, "  \"${CMAKE_CURRENT_LIST_DIR}/%s\"\n", sources.names[row]);
    }
    lhg_text_adds(&cmake, "  \"${CMAKE_CURRENT_LIST_DIR}/dispatch_table.c\"\n)\n");
    lhg_write_if_changed(ctx, lhg_join_path(arena, output, "sources.cmake"), cmake.data, cmake.size);
    lhg_text_free(&cmake);

    stale_scan scan = {output, &unit_arena, &sources};
    lhg_list_directory(ctx, output, remove_stale, &scan);

    result->functions = functions->count;
    result->instructions = main_instructions;
    result->jump_tables = jump_table_count;

    lhg_free(ctx, (void *)sources.names);
    lhg_text_free(&raw);
    lhg_text_free(&body);
    lhg_text_free(&unit);
    lhg_text_free(&declarations);
    lhg_text_free(&tables);
    lhg_text_free(&profiles);
    lhg_arena_release(&unit_arena);
    lhg_arena_release(&function_arena);
    lhg_arena_release(&json_arena);
    lhg_free(ctx, sets);
}
