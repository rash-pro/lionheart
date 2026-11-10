#include "discover.h"

#include <stdlib.h>
#include <string.h>

int lhg_image_contains(const lhg_image *image, int64_t pc)
{
    return pc % 4 == 0 && (int64_t)image->base <= pc && pc + 4 <= (int64_t)image->base + (int64_t)image->size;
}

int lhg_image_executable(const lhg_image *image, int64_t pc)
{
    if (!lhg_image_contains(image, pc))
        return 0;
    if (!image->code_range_count)
        return 1;
    for (size_t index = 0; index < image->code_range_count; index++)
        if (image->code_ranges[index].low <= pc && pc < image->code_ranges[index].high)
            return 1;
    return 0;
}

uint32_t lhg_image_word(const lhg_image *image, int64_t pc)
{
    const uint8_t *at = image->data + (size_t)(pc - (int64_t)image->base);
    return (uint32_t)at[0] | (uint32_t)at[1] << 8 | (uint32_t)at[2] << 16 | (uint32_t)at[3] << 24;
}

lhg_insn lhg_image_insn(const lhg_image *image, int64_t pc)
{
    return lhg_decode(lhg_image_word(image, pc), (uint32_t)pc);
}

void lhg_image_validate(lhg_ctx *ctx, const lhg_image *image)
{
    if ((uint64_t)image->base + image->size > UINT64_C(1) << 32)
        lhg_fail(ctx, "image extends past the 32-bit address space");
}

int lhg_entry_hint_valid(const lhg_image *image, int64_t pc)
{
    if (!lhg_image_executable(image, pc))
        return 0;
    lhg_insn i = lhg_image_insn(image, pc);
    return i.word && i.op != LHG_OP_RESERVED && i.op != LHG_OP_COP_UNUSABLE;
}

static size_t word_index(const lhg_image *image, int64_t pc)
{
    return (size_t)(pc - (int64_t)(image->base & ~3u)) / 4;
}

typedef struct {
    int64_t *items;
    size_t count, capacity;
} pc_stack;

static void push(lhg_ctx *ctx, pc_stack *stack, int64_t value)
{
    stack->items = lhg_grow(ctx, stack->items, &stack->capacity, stack->count, sizeof *stack->items);
    stack->items[stack->count++] = value;
}

static int compare_u32(const void *a, const void *b)
{
    uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;
    return x < y ? -1 : x > y;
}

static int compare_insn(const void *a, const void *b)
{
    uint32_t x = ((const lhg_insn *)a)->pc, y = ((const lhg_insn *)b)->pc;
    return x < y ? -1 : x > y;
}

static int compare_table(const void *a, const void *b)
{
    uint32_t x = (*(lhg_jump_table *const *)a)->pc, y = (*(lhg_jump_table *const *)b)->pc;
    return x < y ? -1 : x > y;
}

static int compare_function(const void *a, const void *b)
{
    uint32_t x = ((const lhg_function *)a)->entry, y = ((const lhg_function *)b)->entry;
    return x < y ? -1 : x > y;
}

static size_t sort_unique_u32(uint32_t *values, size_t count)
{
    if (!count)
        return 0;
    qsort(values, count, sizeof *values, compare_u32);
    size_t used = 1;
    for (size_t index = 1; index < count; index++)
        if (values[index] != values[used - 1])
            values[used++] = values[index];
    return used;
}

/* Recognize Psy-Q's lui/addiu/addu/lw/nop/jr indexed table idiom. */
static size_t jump_table(lhg_ctx *ctx, const lhg_image *image, int64_t pc, uint32_t **words, size_t *word_count,
                         size_t *word_capacity, uint32_t *targets)
{
    int64_t start = pc - 40 > (int64_t)image->base ? pc - 40 : (int64_t)image->base;
    lhg_insn history[16];
    int used = 0;
    for (int64_t p = start; p < pc; p += 4)
        history[used++] = lhg_image_insn(image, p);
    int reg = lhg_image_insn(image, pc).rs;
    const lhg_insn *load = NULL;
    for (int index = used - 1; index >= 0; index--) {
        if (history[index].op == LHG_OP_LW && history[index].rt == reg) {
            load = &history[index];
            break;
        }
    }
    if (!load || pc - (int64_t)load->pc > 12)
        return 0;
    uint32_t constants[32], bases[32];
    uint8_t known[32] = {1}, has_base[32] = {0};
    constants[0] = 0;
    for (int index = 0; index < used; index++) {
        const lhg_insn *i = &history[index];
        if (i->pc == load->pc) {
            uint32_t base;
            if (known[i->rs])
                base = constants[i->rs];
            else if (has_base[i->rs])
                base = bases[i->rs];
            else
                return 0;
            int64_t table = (uint32_t)((int64_t)base + lhg_insn_signed_imm(i));
            uint32_t found = 0;
            for (int entry = 0; entry < 256; entry++) {
                int64_t address = table + (int64_t)entry * 4;
                if (!lhg_image_contains(image, address))
                    break;
                uint32_t target = lhg_image_word(image, address);
                int64_t distance = (int64_t)target - pc;
                if (distance < 0)
                    distance = -distance;
                if (!lhg_image_executable(image, (int64_t)target) || distance > 0x10000)
                    break;
                targets[found++] = target;
            }
            if (found < 2)
                return 0;
            for (uint32_t entry = 0; entry < found; entry++) {
                *words = lhg_grow(ctx, *words, word_capacity, *word_count, sizeof **words);
                (*words)[(*word_count)++] = (uint32_t)(table + (int64_t)entry * 4);
            }
            return sort_unique_u32(targets, found);
        }
        switch (i->op) {
        case LHG_OP_LUI:
            constants[i->rt] = (uint32_t)i->imm << 16;
            known[i->rt] = 1;
            break;
        case LHG_OP_ADDIU:
        case LHG_OP_ADDI:
            if (known[i->rs]) {
                constants[i->rt] = (uint32_t)((int64_t)constants[i->rs] + lhg_insn_signed_imm(i));
                known[i->rt] = 1;
            }
            break;
        case LHG_OP_ORI:
            if (known[i->rs]) {
                constants[i->rt] = constants[i->rs] | i->imm;
                known[i->rt] = 1;
            }
            break;
        case LHG_OP_ADDU:
        case LHG_OP_ADD: {
            int count = known[i->rs] + known[i->rt];
            if (count == 1) {
                bases[i->rd] = known[i->rs] ? constants[i->rs] : constants[i->rt];
                has_base[i->rd] = 1;
            } else if (count == 2) {
                constants[i->rd] = constants[i->rs] + constants[i->rt];
                known[i->rd] = 1;
            } else {
                known[i->rd] = 0;
            }
            break;
        }
        case LHG_OP_SLL:
        case LHG_OP_SRL:
        case LHG_OP_SRA:
        case LHG_OP_OR:
        case LHG_OP_SUBU:
            known[i->rd] = 0;
            break;
        case LHG_OP_LW:
            known[i->rt] = 0;
            break;
        default:
            break;
        }
    }
    return 0;
}

static int is_zero_run(const uint8_t *at)
{
    for (int index = 0; index < 64; index++)
        if (at[index])
            return 0;
    return 1;
}

void lhg_discover(lhg_arena *arena, const lhg_image *image, const uint32_t *seeds, size_t seed_count, int scan_calls,
                  lhg_functions *functions)
{
    lhg_ctx *ctx = arena->ctx;
    lhg_image_validate(ctx, image);
    size_t words = word_index(image, (int64_t)image->base + (int64_t)image->size + 3) + 1;
    uint8_t *is_entry = lhg_zalloc(ctx, words, 1);
    uint8_t *has_function = lhg_zalloc(ctx, words, 1);
    uint32_t *stamp = lhg_zalloc(ctx, words, sizeof *stamp);

    pc_stack initial = {0}, pending = {0}, todo = {0};
    /* Candidate calls are validated against the image; traversal, not a fixed
     * address range, decides which bytes belong to each function. */
    if (lhg_image_executable(image, (int64_t)image->entry))
        push(ctx, &initial, (int64_t)image->entry);
    for (size_t index = 0; index < image->entry_point_count; index++)
        if (lhg_image_executable(image, (int64_t)image->entry_points[index]))
            push(ctx, &initial, (int64_t)image->entry_points[index]);
    for (size_t index = 0; index < seed_count; index++)
        if (lhg_image_executable(image, (int64_t)seeds[index]))
            push(ctx, &initial, (int64_t)seeds[index]);
    if (scan_calls && image->size >= 4) {
        for (int64_t pc = (int64_t)image->base; pc < (int64_t)image->base + (int64_t)image->size - 3; pc += 4) {
            if (!lhg_image_executable(image, pc))
                continue;
            lhg_insn i = lhg_image_insn(image, pc);
            if (i.op == LHG_OP_JAL && lhg_image_executable(image, (int64_t)lhg_insn_target(&i)))
                push(ctx, &initial, (int64_t)lhg_insn_target(&i));
        }
    }
    uint32_t *sorted = lhg_alloc(ctx, (initial.count + 1) * sizeof *sorted);
    for (size_t index = 0; index < initial.count; index++) {
        sorted[index] = (uint32_t)initial.items[index];
        is_entry[word_index(image, initial.items[index])] = 1;
    }
    size_t sorted_count = sort_unique_u32(sorted, initial.count);
    for (size_t index = 0; index < sorted_count; index++)
        push(ctx, &pending, (int64_t)sorted[index]);

    lhg_function *found = NULL;
    size_t found_count = 0, found_capacity = 0;
    lhg_insn *insns = NULL;
    size_t insn_count = 0, insn_capacity = 0;
    lhg_jump_table *tables = NULL;
    size_t table_count = 0, table_capacity = 0;
    uint32_t *table_words = NULL;
    size_t table_word_count = 0, table_word_capacity = 0;
    uint32_t generation = 0;
    uint32_t targets[256];

    while (pending.count) {
        int64_t entry = pending.items[--pending.count];
        if (has_function[word_index(image, entry)])
            continue;
        has_function[word_index(image, entry)] = 1;
        generation++;
        insn_count = table_count = table_word_count = 0;
        todo.count = 0;
        push(ctx, &todo, entry);
        while (todo.count) {
            int64_t pc = todo.items[--todo.count];
            if (!lhg_image_executable(image, pc))
                continue;
            size_t at = word_index(image, pc);
            if (stamp[at] == generation)
                continue;
            if (pc != entry && is_entry[at])
                continue;
            /* Do not translate zero-filled buffers as hundreds of thousands of
             * NOPs. A real transfer there remains covered by the interpreter. */
            if (lhg_image_contains(image, pc + 60) && is_zero_run(image->data + (size_t)(pc - (int64_t)image->base)))
                continue;
            lhg_insn i = lhg_image_insn(image, pc);
            insns = lhg_grow(ctx, insns, &insn_capacity, insn_count, sizeof *insns);
            insns[insn_count++] = i;
            stamp[at] = generation;
            uint16_t flags = lhg_op_flags[i.op];
            if (flags & (LHG_F_CONTROL | LHG_F_COPBRANCH)) {
                if (lhg_image_executable(image, pc + 4) && stamp[word_index(image, pc + 4)] != generation) {
                    stamp[word_index(image, pc + 4)] = generation;
                    insns = lhg_grow(ctx, insns, &insn_capacity, insn_count, sizeof *insns);
                    insns[insn_count++] = lhg_image_insn(image, pc + 4);
                }
                uint32_t target = lhg_insn_target(&i);
                if (i.op == LHG_OP_JAL || i.op == LHG_OP_BLTZAL || i.op == LHG_OP_BGEZAL) {
                    if (lhg_image_executable(image, (int64_t)target) && !is_entry[word_index(image, (int64_t)target)]) {
                        is_entry[word_index(image, (int64_t)target)] = 1;
                        push(ctx, &pending, (int64_t)target);
                    }
                    push(ctx, &todo, pc + 8);
                } else if (flags & LHG_F_BRANCH || flags & LHG_F_COPBRANCH) {
                    push(ctx, &todo, (int64_t)target);
                    push(ctx, &todo, pc + 8);
                } else if (i.op == LHG_OP_J) {
                    push(ctx, &todo, (int64_t)target);
                } else if (i.op == LHG_OP_JALR) {
                    push(ctx, &todo, pc + 8);
                } else if (i.rs != 31) {
                    size_t count = jump_table(ctx, image, pc, &table_words, &table_word_count, &table_word_capacity, targets);
                    if (count) {
                        tables = lhg_grow(ctx, tables, &table_capacity, table_count, sizeof *tables);
                        tables[table_count].pc = (uint32_t)pc;
                        tables[table_count].count = (uint32_t)count;
                        tables[table_count].targets = lhg_arena_alloc(arena, count * sizeof *targets);
                        memcpy(tables[table_count].targets, targets, count * sizeof *targets);
                        table_count++;
                        for (size_t index = 0; index < count; index++)
                            push(ctx, &todo, (int64_t)targets[index]);
                    }
                }
            } else if (!(flags & LHG_F_STOP)) {
                push(ctx, &todo, pc + 4);
            }
        }
        if (!insn_count)
            continue;
        found = lhg_grow(ctx, found, &found_capacity, found_count, sizeof *found);
        lhg_function *f = &found[found_count++];
        memset(f, 0, sizeof *f);
        f->entry = (uint32_t)entry;
        qsort(insns, insn_count, sizeof *insns, compare_insn);
        f->insn_count = (uint32_t)insn_count;
        f->insns = lhg_arena_alloc(arena, insn_count * sizeof *insns);
        memcpy(f->insns, insns, insn_count * sizeof *insns);
        f->table_count = (uint32_t)table_count;
        if (table_count) {
            f->tables = lhg_arena_alloc(arena, table_count * sizeof *tables);
            memcpy(f->tables, tables, table_count * sizeof *tables);
            f->tables_by_pc = lhg_arena_alloc(arena, table_count * sizeof *f->tables_by_pc);
            for (size_t index = 0; index < table_count; index++)
                f->tables_by_pc[index] = &f->tables[index];
            qsort(f->tables_by_pc, table_count, sizeof *f->tables_by_pc, compare_table);
        }
        size_t unique_words = sort_unique_u32(table_words, table_word_count);
        f->table_word_count = (uint32_t)unique_words;
        if (unique_words) {
            f->table_words = lhg_arena_alloc(arena, unique_words * sizeof *table_words);
            memcpy(f->table_words, table_words, unique_words * sizeof *table_words);
        }
    }
    lhg_function *items = lhg_arena_alloc(arena, (found_count + 1) * sizeof *items);
    if (found_count)
        memcpy(items, found, found_count * sizeof *items);
    /* A newly found branch-and-link entry might already be in an older body;
     * overlap is harmless because each entry has its own reachable translation. */
    if (found_count)
        qsort(items, found_count, sizeof *items, compare_function);
    functions->items = items;
    functions->count = found_count;
    lhg_free(ctx, is_entry);
    lhg_free(ctx, has_function);
    lhg_free(ctx, stamp);
    lhg_free(ctx, initial.items);
    lhg_free(ctx, pending.items);
    lhg_free(ctx, todo.items);
    lhg_free(ctx, sorted);
    lhg_free(ctx, found);
    lhg_free(ctx, insns);
    lhg_free(ctx, tables);
    lhg_free(ctx, table_words);
}

const lhg_function *lhg_functions_find(const lhg_functions *functions, uint32_t pc)
{
    size_t low = 0, high = functions->count;
    while (low < high) {
        size_t middle = low + (high - low) / 2;
        uint32_t value = functions->items[middle].entry;
        if (value == pc)
            return &functions->items[middle];
        if (value < pc)
            low = middle + 1;
        else
            high = middle;
    }
    return NULL;
}

const lhg_insn *lhg_function_insn(const lhg_function *function, int64_t pc)
{
    if (pc < 0 || pc > (int64_t)UINT32_MAX)
        return NULL;
    uint32_t want = (uint32_t)pc;
    uint32_t low = 0, high = function->insn_count;
    while (low < high) {
        uint32_t middle = low + (high - low) / 2;
        uint32_t value = function->insns[middle].pc;
        if (value == want)
            return &function->insns[middle];
        if (value < want)
            low = middle + 1;
        else
            high = middle;
    }
    return NULL;
}

const lhg_jump_table *lhg_function_table(const lhg_function *function, int64_t pc)
{
    uint32_t low = 0, high = function->table_count;
    while (low < high) {
        uint32_t middle = low + (high - low) / 2;
        int64_t value = function->tables_by_pc[middle]->pc;
        if (value == pc)
            return function->tables_by_pc[middle];
        if (value < pc)
            low = middle + 1;
        else
            high = middle;
    }
    return NULL;
}

uint32_t *lhg_prologue_seeds(lhg_arena *arena, const uint8_t *data, size_t size, uint32_t base, size_t *count)
{
    size_t capacity = size / 4 + 1, used = 0;
    uint32_t *seeds = lhg_arena_alloc(arena, capacity * sizeof *seeds);
    for (size_t offset = 0; offset + 3 < size; offset += 4) {
        uint32_t word = (uint32_t)data[offset] | (uint32_t)data[offset + 1] << 8 | (uint32_t)data[offset + 2] << 16 |
                        (uint32_t)data[offset + 3] << 24;
        /* addiu sp, sp, -N: the signed low half is negative. */
        if (word >> 16 == 0x27BD && (word & 0x8000))
            seeds[used++] = (uint32_t)(base + offset);
    }
    *count = used;
    return seeds;
}
