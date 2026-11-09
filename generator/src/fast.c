/* Basic-block emission with statically specialized one-instruction load delays.
 *
 * The pending register is part of a block's translation key, never a runtime tag
 * in the common path. Only entry from another function/interpreter can have an
 * unknown tag (-1); that alternate first instruction uses the trace retirement.
 *
 * The functions here append to a line list in the order the output needs. Label
 * requests enqueue blocks as a side effect, so their call order is part of the
 * generated text. */
#include "emit.h"

#include <string.h>

const lhg_function *lhg_available_find(const lhg_available *available, uint32_t pc)
{
    const lhg_function *found = lhg_functions_find(available->own, pc);
    if (!found && available->main)
        found = lhg_functions_find(available->main, pc);
    return found;
}

void lhg_analyze(lhg_arena *arena, const lhg_function *f, lhg_analysis *analysis)
{
    uint32_t n = f->insn_count;
    analysis->function = f;
    analysis->distance = lhg_arena_alloc(arena, (n + 1) * sizeof *analysis->distance);
    analysis->is_leader = lhg_arena_alloc(arena, n + 1);
    analysis->windows = lhg_arena_alloc(arena, (n + 1) * sizeof *analysis->windows);
    analysis->full_entries = lhg_arena_alloc(arena, n + 1);
    analysis->compact_entries = lhg_arena_alloc(arena, n + 1);
    memset(analysis->is_leader, 0, n + 1);
    memset(analysis->full_entries, 0, n + 1);
    memset(analysis->compact_entries, 0, n + 1);

    int32_t gap = 1;
    int any = 0;
    for (uint32_t index = n; index-- > 0;) {
        const lhg_insn *i = &f->insns[index];
        int64_t pc = i->pc;
        uint16_t flags = lhg_op_flags[i->op];
        int32_t value;
        if (flags & LHG_F_MEMORY) {
            value = 1;
        } else if (lhg_insn_flow(i)) {
            const lhg_insn *slot = lhg_function_insn(f, pc + 4);
            int64_t candidates[2];
            int count = 0;
            if (!(flags & LHG_F_CALL) && i->op != LHG_OP_JR && !(slot && (lhg_op_flags[slot->op] & LHG_F_MEMORY)))
                candidates[count++] = (int64_t)lhg_insn_target(i);
            if (i->op != LHG_OP_J && i->op != LHG_OP_JR && !(flags & LHG_F_CALL))
                candidates[count++] = pc + 8;
            int32_t best = 0;
            for (int choice = 0; choice < count; choice++) {
                if (candidates[choice] <= pc)
                    continue;
                const lhg_insn *next = lhg_function_insn(f, candidates[choice]);
                if (next && analysis->distance[next - f->insns] > best)
                    best = analysis->distance[next - f->insns];
            }
            value = 2 + best;
        } else {
            value = 1 + (index + 1 < n && (int64_t)f->insns[index + 1].pc == pc + 4 ? analysis->distance[index + 1] : 0);
        }
        analysis->distance[index] = value;
        if (!any || value > gap)
            gap = value;
        any = 1;
    }
    analysis->event_gap = gap;

    const lhg_insn *entry_insn = lhg_function_insn(f, (int64_t)f->entry);
    if (entry_insn)
        analysis->is_leader[entry_insn - f->insns] = 1;
    for (uint32_t index = 0; index < n; index++) {
        const lhg_insn *i = &f->insns[index];
        if (!lhg_insn_flow(i))
            continue;
        const lhg_insn *target = lhg_function_insn(f, (int64_t)lhg_insn_target(i));
        if (target)
            analysis->is_leader[target - f->insns] = 1;
        const lhg_insn *after = lhg_function_insn(f, (int64_t)i->pc + 8);
        if (after)
            analysis->is_leader[after - f->insns] = 1;
        const lhg_jump_table *table = lhg_function_table(f, (int64_t)i->pc);
        if (table) {
            for (uint32_t row = 0; row < table->count; row++) {
                const lhg_insn *case_insn = lhg_function_insn(f, (int64_t)table->targets[row]);
                if (case_insn)
                    analysis->is_leader[case_insn - f->insns] = 1;
            }
        }
    }
    for (uint32_t index = n; index-- > 0;) {
        const lhg_insn *i = &f->insns[index];
        int64_t pc = i->pc;
        uint16_t flags = lhg_op_flags[i->op];
        int32_t value;
        if (lhg_insn_flow(i)) {
            value = 2;
        } else if (flags & LHG_F_MEMORY) {
            value = 0;
        } else {
            value = 1;
            if (index + 1 < n && (int64_t)f->insns[index + 1].pc == pc + 4 && !analysis->is_leader[index + 1])
                value += analysis->windows[index + 1];
        }
        analysis->windows[index] = value;
    }
    for (uint32_t index = 0; index < n; index++) {
        if (analysis->is_leader[index]) {
            analysis->full_entries[index] = 1;
            analysis->compact_entries[index] = 1;
        }
        const lhg_insn *i = &f->insns[index];
        if (lhg_op_flags[i->op] & LHG_F_CALL) {
            const lhg_insn *back = lhg_function_insn(f, (int64_t)i->pc + 8);
            if (back) {
                analysis->full_entries[back - f->insns] = 1;
                analysis->compact_entries[back - f->insns] = 1;
            }
        }
    }
    const lhg_insn *entry_next = lhg_function_insn(f, (int64_t)f->entry + 4);
    if (entry_next)
        analysis->full_entries[entry_next - f->insns] = 1;
}

typedef struct {
    int32_t index;
    int pending;
} block_key;

typedef struct {
    lhg_arena *arena;
    const lhg_analysis *analysis;
    const lhg_function *f;
    const lhg_available *available;
    int trace, compact, poll_forward, direct_memory;
    int64_t block_start;
    const uint8_t *entries;
    uint64_t *seen;
    block_key *todo;
    size_t todo_count, todo_capacity, todo_head;
} emitter;

static const lhg_insn *body_at(const emitter *e, int64_t pc)
{
    return lhg_function_insn(e->f, pc);
}

static const char *label(emitter *e, int64_t pc, int pending)
{
    if (e->trace)
        pending = 0;
    const lhg_insn *i = body_at(e, pc);
    if (!i)
        lhg_fail(e->arena->ctx, "internal error: block outside its function");
    int32_t index = (int32_t)(i - e->f->insns);
    uint64_t bit = UINT64_C(1) << (pending + 1);
    if (!(e->seen[index] & bit)) {
        e->seen[index] |= bit;
        if (e->todo_count == e->todo_capacity) {
            size_t capacity = e->todo_capacity ? e->todo_capacity * 2 : 16;
            block_key *grown = lhg_arena_alloc(e->arena, capacity * sizeof *grown);
            if (e->todo_count)
                memcpy(grown, e->todo, e->todo_count * sizeof *grown);
            e->todo = grown;
            e->todo_capacity = capacity;
        }
        e->todo[e->todo_count].index = index;
        e->todo[e->todo_count].pending = pending;
        e->todo_count++;
    }
    if (pending == -1)
        return lhg_fmt(e->arena, "L_%X_incoming", (uint32_t)pc);
    return lhg_fmt(e->arena, "L_%X_%d", (uint32_t)pc, pending);
}

static int window_at(const emitter *e, int64_t pc)
{
    const lhg_insn *i = body_at(e, pc);
    return i ? e->analysis->windows[i - e->f->insns] : 0;
}

static void export_pending(emitter *e, lhg_lines *out, int pending)
{
    if (e->trace || pending <= 0)
        return;
    lhg_lines_addf(out, "c->load_reg = %d; c->load_value = delayed;", pending);
}

static void observe(emitter *e, lhg_lines *out, const lhg_insn *i, int pending, int count, int64_t branch, int advance)
{
    lhg_lines_addf(out, "c->pc = %U;", i->pc);
    lhg_lines_addf(out, "c->in_delay_slot = %d;", branch != 0);
    if (branch)
        lhg_lines_addf(out, "c->branch_pc = %U;", (uint32_t)branch);
    export_pending(e, out, pending);
    if (advance && !e->trace)
        lhg_lines_addf(out, "c_count_recompiled(c, %U, %d - ticked);", e->f->entry, count - 1);
    if (advance) {
        lhg_lines_addf(out, "c_recomp_budget(c, %d - ticked);", count - 1);
        lhg_lines_addf(out, "c->cycles += (uint64_t)(%d - ticked) * CYCLES_PER_INSN;", count - 1);
        lhg_lines_addf(out, "ticked = %d;", count - 1);
        lhg_lines_add(out, "c_tick(c, 0);");
    }
    lhg_lines_addf(out, "c->fault_instructions = 1; c->fault_counted = %d;", e->trace ? 2 : 0);
}

static void observed(emitter *e, lhg_lines *out, int pending)
{
    lhg_lines_add(out, "c->fault_instructions = 0; c->in_delay_slot = 0;");
    if (pending > 0 && !e->trace)
        lhg_lines_add(out, "c->load_reg = c->load_value = 0;");
}

static void retire(lhg_lines *out, int pending, int dst, const char *value, int load, const char *loaded, int defer)
{
    if (pending == -1) {
        if (load) {
            lhg_lines_addf(out, "c_retire(c, %d, %s, %d, %s);", dst, value, load, loaded);
            lhg_lines_add(out, "delayed = c->load_value; c->load_reg = c->load_value = 0;");
            if (!defer)
                lhg_lines_addf(out, "c->r[%d] = delayed;", load);
        } else {
            lhg_lines_addf(out, "c_retire(c, %d, %s, 0, 0);", dst, value);
        }
        return;
    }
    if (pending && pending != dst && pending != load)
        lhg_lines_addf(out, "c->r[%d] = delayed;", pending);
    if (dst)
        lhg_lines_addf(out, "c->r[%d] = %s;", dst, value);
    if (load) {
        if (defer)
            lhg_lines_addf(out, "delayed = %s;", loaded);
        else
            lhg_lines_addf(out, "c->r[%d] = %s;", load, loaded);
    }
}

static int memory_bytes(lhg_op op)
{
    switch (op) {
    case LHG_OP_LB:
    case LHG_OP_LBU:
    case LHG_OP_SB: return 1;
    case LHG_OP_LH:
    case LHG_OP_LHU:
    case LHG_OP_SH: return 2;
    default: return 4;
    }
}

static const char *window_text(emitter *e, const lhg_insn *i, int64_t branch)
{
    if (e->compact)
        return lhg_fmt(e->arena, "((c->cop0[12] & 0x401) == 0x401 ? %du : 0u)", e->analysis->event_gap);
    return lhg_fmt(e->arena, "%d", branch ? 0 : window_at(e, (int64_t)i->pc + 4));
}

/* Emits the common memory access; returns the expression for the loaded value. */
static const char *memory_access(emitter *e, lhg_lines *out, const lhg_insn *i, int pending, int count, int64_t branch)
{
    lhg_op op = (lhg_op)i->op;
    int width = memory_bytes(op);
    int load = (lhg_op_flags[op] & LHG_F_LOAD) != 0;
    int partial = op == LHG_OP_LWL || op == LHG_OP_LWR;
    lhg_lines_addf(out, "uint32_t address = a + %U;", (uint32_t)(int32_t)lhg_insn_signed_imm(i));
    lhg_lines_addf(out, "uint32_t aligned = %s;", partial ? "address & ~3u" : "address");
    lhg_lines_add(out, "uint32_t physical = aligned & 0x1fffffffu;");
    if (load)
        lhg_lines_add(out, "uint32_t memory;");
    lhg_lines_addf(out, "int timed = physical >= c->ram_limit || (aligned & %du) || (int64_t)(c->cycles + ((uint64_t)(%d - ticked) + %s) * CYCLES_PER_INSN - c->next_event) >= 0;",
                   width - 1, count, window_text(e, i, branch));
    lhg_lines_add(out, "if (!timed) {");
    if (load)
        lhg_lines_addf(out, "memory = ram_read(c, physical, %d);", width);
    else
        lhg_lines_addf(out, "ram_write(c, physical, %d, b);", width);
    lhg_lines_add(out, "} else {");
    observe(e, out, i, pending, count, branch, 0);
    if (load)
        lhg_lines_addf(out, "memory = mem_compiled_read(c, aligned, %d, %d - ticked);", width, count - 1);
    else
        lhg_lines_addf(out, "mem_compiled_write(c, aligned, %d, b, %d - ticked);", width, count - 1);
    lhg_lines_addf(out, "ticked = %d;", count - 1);
    observed(e, out, pending);
    lhg_lines_add(out, "}");
    const char *cast = op == LHG_OP_LB ? "(uint32_t)(int32_t)(int8_t)" : op == LHG_OP_LH ? "(uint32_t)(int32_t)(int16_t)" : "";
    const char *value = lhg_fmt(e->arena, "%smemory", cast);
    if (partial) {
        const char *old;
        if (pending == i->rt && pending > 0)
            old = "delayed";
        else if (pending == -1)
            old = lhg_fmt(e->arena, "c_merge_value(c, %d)", (int)i->rt);
        else
            old = lhg_fmt(e->arena, "c->r[%d]", (int)i->rt);
        const char *direction = op == LHG_OP_LWL ? "<<" : ">>";
        lhg_lines_addf(out, "unsigned shift = %s;", op == LHG_OP_LWL ? "(3 - (address & 3)) * 8" : "(address & 3) * 8");
        lhg_lines_addf(out, "uint32_t mask = 0xffffffffu %s shift;", direction);
        value = lhg_fmt(e->arena, "(%s & ~mask) | (memory %s shift)", old, direction);
    }
    return value;
}

static int direct_instruction(emitter *e, lhg_lines *out, const lhg_insn *i, int pending, int count, int64_t branch)
{
    lhg_op op = (lhg_op)i->op;
    int is_load = (lhg_op_flags[op] & LHG_F_LOAD) != 0;
    const char *helper = op == LHG_OP_LW ? "c_memory_lw" : op == LHG_OP_SW ? "c_memory_sw" : "c_memory";
    int width = memory_bytes(op);
    int load = is_load ? i->rt : 0;
    lhg_lines head;
    lhg_lines_init(&head, e->arena);
    lhg_lines_addf(&head, "uint32_t a = c->r[%d], b = c->r[%d];", (int)i->rs, (int)i->rt);
    lhg_lines_addf(&head, "uint32_t address = a + %U;", (uint32_t)(int32_t)lhg_insn_signed_imm(i));
    lhg_lines_add(&head, "uint32_t physical = address & 0x1fffffffu;");
    lhg_lines_addf(&head, "if (physical < c->ram_limit && !(address & %du) && (int64_t)(c->cycles + ((uint64_t)(%d - ticked) + %s) * CYCLES_PER_INSN - c->next_event) < 0) {",
                   width - 1, count, window_text(e, i, branch));
    lhg_lines_replace(&head, "c->r[0]", "0u");
    lhg_lines_extend(out, &head);
    if (is_load) {
        const char *cast = op == LHG_OP_LB ? "(uint32_t)(int32_t)(int8_t)" : op == LHG_OP_LH ? "(uint32_t)(int32_t)(int16_t)" : "";
        lhg_lines_addf(out, "uint32_t memory = %sram_read(c, physical, %d);", cast, width);
        retire(out, pending, 0, "0", load, "memory", load != 0);
        if (!load)
            lhg_lines_add(out, "(void)memory;");
    } else {
        if (op == LHG_OP_LWC2) {
            lhg_lines_addf(out, "gte_write(c, %d, ram_read(c, physical, 4), 0);", (int)i->rt);
        } else if (op == LHG_OP_SWC2) {
            lhg_lines_addf(out, "ram_write(c, physical, %d, gte_read(c, %d, 0));", width, (int)i->rt);
        } else {
            lhg_lines_addf(out, "ram_write(c, physical, %d, b);", width);
        }
        retire(out, pending, 0, "0", 0, "0", 0);
    }
    lhg_lines_add(out, "} else {");
    lhg_lines_addf(out, "static const memory_site_t site = {%U, %U, %d, %U, %d, %d, %d, %d, %d, &code_%X};",
                   i->pc, (uint32_t)branch, count - 1, (uint32_t)(int32_t)lhg_insn_signed_imm(i), pending, (int)i->rs, (int)i->rt,
                   (int)(i->word >> 26), load != 0, e->f->entry);
    lhg_lines_addf(out, "%s%s(c, &site, delayed, ticked);", load ? "delayed = " : "", helper);
    lhg_lines_add(out, "if (c->memory_exit) return;");
    lhg_lines_add(out, "ticked = c->memory_ticked;");
    lhg_lines_add(out, "}");
    return load;
}

static int instruction(emitter *e, lhg_lines *out, const lhg_insn *i, int pending, int count, int64_t branch)
{
    lhg_op op = (lhg_op)i->op;
    uint16_t flags = lhg_op_flags[op];
    if (e->trace) {
        /* Timing/fault context is block based even in diagnostic mode. */
        observe(e, out, i, 0, count, branch, 1);
        lhg_operation(e->arena, out, i);
        observed(e, out, 0);
        return 0;
    }
    const char *helper = op == LHG_OP_LW ? "c_memory_lw" : op == LHG_OP_SW ? "c_memory_sw" : "c_memory";
    if ((flags & LHG_F_DIRECT_MEMORY) && (!e->compact || e->direct_memory))
        return direct_instruction(e, out, i, pending, count, branch);
    if (e->compact && (flags & LHG_F_MEMORY)) {
        int load = (flags & LHG_F_LOAD) ? i->rt : 0;
        int defer = load != 0;
        lhg_lines_addf(out, "static const memory_site_t site = {%U, %U, %d, %U, %d, %d, %d, %d, %d, &code_%X};",
                       i->pc, (uint32_t)branch, count - 1, (uint32_t)(int32_t)lhg_insn_signed_imm(i), pending, (int)i->rs, (int)i->rt,
                       (int)(i->word >> 26), defer, e->f->entry);
        lhg_lines_addf(out, "%s%s(c, &site, delayed, ticked);", defer ? "delayed = " : "", helper);
        lhg_lines_add(out, "if (c->memory_exit) return;");
        lhg_lines_add(out, "ticked = c->memory_ticked;");
        return defer ? load : 0;
    }
    lhg_effect effect;
    lhg_effects(e->arena, i, &effect);
    /* Reads of r0 are constants; writes are discarded below. */
    lhg_lines_replace(&effect.lines, "c->r[0]", "0u");
    lhg_lines lines = effect.lines;
    int dst = effect.dst, load = effect.load;
    const char *value = effect.value, *loaded = effect.loaded;
    if (((flags & LHG_F_LOAD) && (flags & LHG_F_MEMORY)) || op == LHG_OP_SB || op == LHG_OP_SH || op == LHG_OP_SW) {
        lines.count = 1;
        loaded = memory_access(e, &lines, i, pending, count, branch);
    } else if (op == LHG_OP_ADD || op == LHG_OP_ADDI || op == LHG_OP_SUB) {
        const char *operand = op == LHG_OP_ADDI ? lhg_fmt(e->arena, "%U", (uint32_t)(int32_t)lhg_insn_signed_imm(i)) : "b";
        const char *sign = op == LHG_OP_SUB ? "-" : "+";
        const char *overflow = op == LHG_OP_SUB ? lhg_fmt(e->arena, "((a ^ %s) & (a ^ value))", operand) : lhg_fmt(e->arena, "(~(a ^ %s) & (a ^ value))", operand);
        lhg_lines_addf(&lines, "uint32_t value = a %s %s;", sign, operand);
        lhg_lines_addf(&lines, "if (%s & 0x80000000u) {", overflow);
        observe(e, &lines, i, pending, count, branch, 1);
        lhg_lines_add(&lines, "cpu_exception(c, 12, 0);");
        lhg_lines_add(&lines, "return;");
        lhg_lines_add(&lines, "}");
        value = "value";
    } else if (op == LHG_OP_SYSCALL) {
        retire(&lines, pending, 0, "0", 0, "0", 0);
        observe(e, &lines, i, 0, count, branch, 1);
        lhg_lines_add(&lines, "bios_syscall(c);");
        observed(e, &lines, 0);
        lhg_lines_extend(out, &lines);
        return 0;
    } else if ((flags & LHG_F_OBSERVER) || op == LHG_OP_MFC2 || op == LHG_OP_CFC2 || op == LHG_OP_SWL || op == LHG_OP_SWR) {
        lhg_lines first = lines;
        first.count = 1;
        lhg_lines rebuilt;
        lhg_lines_init(&rebuilt, e->arena);
        lhg_lines_extend(&rebuilt, &first);
        observe(e, &rebuilt, i, pending, count, branch, 1);
        lhg_lines rest = lines;
        rest.items = lines.items + 1;
        rest.count = lines.count - 1;
        lhg_lines_extend(&rebuilt, &rest);
        lhg_lines_addf(&rebuilt, "uint32_t loaded = %s;", loaded);
        observed(e, &rebuilt, pending);
        loaded = "loaded";
        if (flags & LHG_F_STOP) {
            lhg_lines_add(&rebuilt, "return;");
            lhg_lines_extend(out, &rebuilt);
            return 0;
        }
        lines = rebuilt;
    }
    /* Capture operands before overwriting either a pending load or its merge. */
    if (dst && strcmp(value, "value"))
        lhg_lines_addf(&lines, "uint32_t value = %s;", value);
    if (dst)
        value = "value";
    if (load && strcmp(loaded, "loaded"))
        lhg_lines_addf(&lines, "uint32_t loaded = %s;", loaded);
    if (load)
        loaded = "loaded";
    /* Loads to r0 still perform memory/coprocessor side effects. */
    if ((flags & LHG_F_LOAD) && !load && strcmp(loaded, "0") && strcmp(loaded, "loaded"))
        lhg_lines_addf(&lines, "(void)(%s);", loaded);
    /* IRQ handlers can observe a pending load even when the next guest
     * instruction does not read it. Preserve the architectural delay. */
    int defer = load != 0;
    retire(&lines, pending, dst, value, load, loaded, defer);
    lhg_lines_extend(out, &lines);
    return defer ? load : 0;
}

/* pc is the numeric target unless expr supplies a runtime expression; window
 * lookup by expression yields zero, matching a miss in the window table. */
static void tick(emitter *e, lhg_lines *out, int64_t pc, const char *expr, int count, int pending, int reset, const char *window)
{
    if (e->compact) {
        const char *target = expr ? expr : lhg_fmt(e->arena, "%U", (uint32_t)pc);
        export_pending(e, out, pending);
        lhg_lines_addf(out, "if (!c_block(c, %s, %d - ticked, &code_%X)) return;", target, count, e->f->entry);
        lhg_lines_addf(out, "ticked = %d;", reset ? 0 : count);
        lhg_lines_add(out, "code_epoch = c->code_epoch;");
        if (pending > 0)
            lhg_lines_add(out, "c->load_reg = c->load_value = 0;");
        return;
    }
    if (expr)
        lhg_lines_addf(out, "c->pc = %s;", expr);
    else
        lhg_lines_addf(out, "c->pc = %U;", (uint32_t)pc);
    export_pending(e, out, pending);
    if (count && !e->trace)
        lhg_lines_addf(out, "c_count_recompiled(c, %U, %d - ticked);", e->f->entry, count);
    if (count)
        lhg_lines_addf(out, "c_recomp_budget(c, %d - ticked);", count);
    if (e->trace) {
        lhg_lines_addf(out, "c_tick(c, %d - ticked); ticked = %d;", count, reset ? 0 : count);
    } else {
        const char *window_value = window ? window : lhg_fmt(e->arena, "%d", expr ? 0 : window_at(e, pc));
        lhg_lines_addf(out, "if (!c_tick_window(c, %d - ticked, %s)) return;", count, window_value);
        lhg_lines_addf(out, "ticked = %d;", reset ? 0 : count);
    }
    lhg_lines_add(out, "if (code_epoch != c->code_epoch) {");
    lhg_lines_addf(out, "if (!code_valid(c, &code_%X)) return;", e->f->entry);
    lhg_lines_add(out, "code_epoch = c->code_epoch;");
    lhg_lines_add(out, "}");
    if (pending > 0 && !e->trace)
        lhg_lines_add(out, "c->load_reg = c->load_value = 0;");
}

static void jump(emitter *e, lhg_lines *out, int64_t target, int count, int pending)
{
    if (e->compact && !e->poll_forward && body_at(e, target) && target > e->block_start) {
        /* Carry the unflushed count into a forward successor. Only entry,
         * back edges and external transfers are polling safe points. */
        lhg_lines_addf(out, "ticked -= %d;", count);
        lhg_lines_addf(out, "goto %s;", label(e, target, pending));
        return;
    }
    tick(e, out, target, NULL, count, pending, 1, NULL);
    if (body_at(e, target)) {
        lhg_lines_addf(out, "goto %s;", label(e, target, pending));
    } else {
        export_pending(e, out, pending);
        lhg_lines_add(out, "return;");
    }
}

static void resume(emitter *e, lhg_lines *out, int64_t pc)
{
    if (!body_at(e, pc)) {
        lhg_lines_add(out, "return;");
        return;
    }
    if (e->trace) {
        lhg_lines_addf(out, "goto %s;", label(e, pc, 0));
        return;
    }
    lhg_lines_addf(out, "if (c->load_reg) goto %s;", label(e, pc, -1));
    lhg_lines_addf(out, "goto %s;", label(e, pc, 0));
}

static void call(emitter *e, lhg_lines *out, const lhg_insn *i, int pending, int count)
{
    int64_t back = (int64_t)i->pc + 8;
    const char *target = i->op == LHG_OP_JALR ? "target" : lhg_fmt(e->arena, "%U", lhg_insn_target(i));
    lhg_lines_addf(out, "c_transfer_stats(c, %U, %s, C_DISPATCH_CALL);", i->pc, target);
    tick(e, out, 0, target, count, pending, 1, NULL);
    export_pending(e, out, pending);
    if (e->compact) {
        lhg_lines_add(out, "return;");
        return;
    }
    lhg_lines_addf(out, "if (!c_enter(c, %s, %U)) return;", target, (uint32_t)back);
    if (i->op != LHG_OP_JALR && lhg_available_find(e->available, lhg_insn_target(i))) {
        lhg_lines_addf(out, "f_%X(c);", lhg_insn_target(i));
        lhg_lines_addf(out, "if (c->pc != %U) dispatch(c, c->pc);", (uint32_t)back);
    } else {
        lhg_lines_addf(out, "dispatch(c, %s);", target);
    }
    lhg_lines_add(out, "c_leave(c);");
    lhg_lines_addf(out, "if (c->pc != %U) return;", (uint32_t)back);
    tick(e, out, back, NULL, 0, 0, 1, NULL);
    resume(e, out, back);
}

/* Recognize a bounded RAM countdown around an IRQ-owned clock read. */
static const char *idle_site(emitter *e, int64_t start)
{
    const lhg_insn *body[14];
    for (int index = 0; index < 8; index++) {
        body[index] = body_at(e, start + index * 4);
        if (!body[index])
            return NULL;
    }
    if (body[6]->op != LHG_OP_BNE)
        return NULL;
    int64_t base = (int64_t)lhg_insn_target(body[6]);
    for (int index = 0; index < 6; index++) {
        body[8 + index] = body_at(e, base + index * 4);
        if (!body[8 + index])
            return NULL;
    }
    static const uint8_t expected[14] = {LHG_OP_LW, LHG_OP_SLL, LHG_OP_ADDIU, LHG_OP_SW, LHG_OP_LW, LHG_OP_SLL, LHG_OP_BNE,
                                         LHG_OP_SLL, LHG_OP_LUI, LHG_OP_LW, LHG_OP_SLL, LHG_OP_SLT, LHG_OP_BNE, LHG_OP_SLL};
    for (int index = 0; index < 14; index++)
        if (body[index]->op != expected[index])
            return NULL;
    const lhg_insn *a = body[0], *nop0 = body[1], *sub = body[2], *store = body[3], *reload = body[4], *nop1 = body[5];
    const lhg_insn *timeout = body[6], *nop2 = body[7], *high = body[8], *clock = body[9], *nop3 = body[10], *less = body[11];
    const lhg_insn *again = body[12], *nop4 = body[13];
    int value = a->rt;
    if (!value || nop0->word || nop1->word || nop2->word || nop3->word || nop4->word)
        return NULL;
    if (!(sub->rs == value && sub->rt == value && store->rt == value && reload->rt == value && high->rt == value &&
          clock->rt == value && clock->rs == value && less->rd == value && less->rs == value && again->rs == value))
        return NULL;
    if (!(a->rs == store->rs && store->rs == reload->rs && a->imm == store->imm && store->imm == reload->imm))
        return NULL;
    if (lhg_insn_signed_imm(sub) != -1 || timeout->rs != value || (int64_t)lhg_insn_target(timeout) < start + 32)
        return NULL;
    if (again->rt || (int64_t)lhg_insn_target(again) != start || value == a->rs || value == less->rt || value == timeout->rt)
        return NULL;
    return lhg_fmt(e->arena, "{%U, %U, %d, %d, %d, %d, %U}", (uint32_t)(int32_t)lhg_insn_signed_imm(a),
                   (uint32_t)(((int64_t)high->imm << 16) + lhg_insn_signed_imm(clock)), (int)a->rs, value, (int)less->rt,
                   (int)timeout->rt, high->pc);
}

/* Prove the battle limiter's eight-instruction, convergent iteration. */
static const char *poll_site(emitter *e, int64_t start)
{
    const lhg_insn *loop[4];
    for (int index = 0; index < 4; index++) {
        loop[index] = body_at(e, start + index * 4);
        if (!loop[index])
            return NULL;
    }
    const lhg_insn *call_insn = loop[0], *slot = loop[1], *again = loop[2], *delay = loop[3];
    if (call_insn->op != LHG_OP_JAL || slot->word || delay->word || again->op != LHG_OP_BNE)
        return NULL;
    if ((int64_t)lhg_insn_target(again) != start || again->rt)
        return NULL;
    const lhg_function *getter = lhg_available_find(e->available, lhg_insn_target(call_insn));
    if (!getter || getter->insn_count != 4)
        return NULL;
    uint32_t base = lhg_insn_target(call_insn);
    for (uint32_t index = 0; index < 4; index++)
        if (getter->insns[index].pc != base + index * 4)
            return NULL;
    const lhg_insn *high = &getter->insns[0], *load = &getter->insns[1], *ret = &getter->insns[2], *nop = &getter->insns[3];
    if (high->op != LHG_OP_LUI || load->op != LHG_OP_LW || ret->op != LHG_OP_JR || nop->op != LHG_OP_SLL || nop->word)
        return NULL;
    int result = high->rt;
    if (result == 0 || result == 31 || load->rs != result || load->rt != result || again->rs != result || ret->rs != 31)
        return NULL;
    uint32_t address = (uint32_t)(((uint64_t)high->imm << 16) + (uint64_t)(int64_t)lhg_insn_signed_imm(load));
    uint32_t physical = address & 0x1fffffffu;
    if (address & 3 || !(physical < 0x800000 || (physical >= 0x1f800000 && physical <= 0x1f8003fc)))
        return NULL;
    return lhg_fmt(e->arena, "{%U, %U, %U, %d, {%U, %U, %U, %U, %U, %U, %U, %U}}", (uint32_t)start, lhg_insn_target(call_insn), address,
                   result, loop[0]->word, loop[1]->word, loop[2]->word, loop[3]->word, high->word, load->word, ret->word, nop->word);
}

static const char *condition_text(emitter *e, const lhg_insn *i)
{
    switch ((lhg_op)i->op) {
    case LHG_OP_BEQ: return lhg_fmt(e->arena, "c->r[%d] == c->r[%d]", (int)i->rs, (int)i->rt);
    case LHG_OP_BNE: return lhg_fmt(e->arena, "c->r[%d] != c->r[%d]", (int)i->rs, (int)i->rt);
    case LHG_OP_BLEZ: return lhg_fmt(e->arena, "(int32_t)c->r[%d] <= 0", (int)i->rs);
    case LHG_OP_BGTZ: return lhg_fmt(e->arena, "(int32_t)c->r[%d] > 0", (int)i->rs);
    case LHG_OP_BLTZ:
    case LHG_OP_BLTZAL: return lhg_fmt(e->arena, "(int32_t)c->r[%d] < 0", (int)i->rs);
    case LHG_OP_BGEZ:
    case LHG_OP_BGEZAL: return lhg_fmt(e->arena, "(int32_t)c->r[%d] >= 0", (int)i->rs);
    case LHG_OP_BC0T:
    case LHG_OP_BC2T: return "0";
    default: return "1";
    }
}

static void block(emitter *e, lhg_lines *out, int64_t start, int pending)
{
    e->block_start = start;
    int64_t pc = start;
    int count = 0;
    if (pending == 0 && !e->trace) {
        const char *idle = idle_site(e, start);
        if (idle) {
            tick(e, out, start, NULL, 0, 0, 1, NULL);
            lhg_lines_addf(out, "static const idle_site_t idle = %s;", idle);
            lhg_lines_add(out, "c_idle(c, &idle);");
            tick(e, out, start, NULL, 0, 0, 1, NULL);
        }
        const char *poll = poll_site(e, start);
        if (poll) {
            tick(e, out, start, NULL, 0, 0, 1, NULL);
            lhg_lines_addf(out, "static const poll_site_t poll = %s;", poll);
            lhg_lines_add(out, "c_poll_skip(c, &poll);");
            tick(e, out, start, NULL, 0, 0, 1, NULL);
        }
    }
    for (;;) {
        const lhg_insn *i = body_at(e, pc);
        if (!i)
            return;
        lhg_op op = (lhg_op)i->op;
        uint16_t flags = lhg_op_flags[op];
        count++;
        lhg_lines_add(out, "{");
        if (!lhg_insn_flow(i)) {
            lhg_lines_addf(out, "c_pc_count(c, %U, 1);", (uint32_t)pc);
            pending = instruction(e, out, i, pending, count, 0);
            int memory_family = (flags & LHG_F_MEMORY) != 0;
            int direct = (flags & LHG_F_DIRECT_MEMORY) != 0;
            int timed_check = (memory_family && (!e->compact || (e->direct_memory && direct)) && (e->trace || !direct)) || op == LHG_OP_SYSCALL;
            if (timed_check) {
                const char *condition;
                if (op == LHG_OP_SYSCALL) {
                    condition = "1";
                } else if ((flags & LHG_F_NORMAL_MEMORY) && !e->trace) {
                    condition = "timed";
                } else {
                    condition = lhg_fmt(e->arena, "((a + %U) & 0x1fffffffu) >= c->ram_limit || (int64_t)(c->cycles + (uint64_t)(%d - ticked) * CYCLES_PER_INSN - c->next_event) >= 0",
                                        (uint32_t)(int32_t)lhg_insn_signed_imm(i), count + window_at(e, pc + 4));
                }
                lhg_lines_addf(out, "if (%s) {", condition);
                tick(e, out, pc + 4, NULL, count, pending, 0, NULL);
                lhg_lines_add(out, "}");
            }
            lhg_lines_add(out, "}");
            if (flags & LHG_F_STOP)
                return;
            pc += 4;
            if (((op == LHG_OP_MTC0 || op == LHG_OP_CTC0) && i->rd == 12) || op == LHG_OP_RFE) {
                tick(e, out, pc, NULL, count, pending, 1, NULL);
                resume(e, out, pc);
                return;
            }
            if (((flags & LHG_F_STORE) || op == LHG_OP_SWC2 || op == LHG_OP_SYSCALL) && (!e->compact || (e->direct_memory && direct))) {
                lhg_lines_addf(out, "if (code_epoch != c->code_epoch && !code_valid(c, &code_%X)) {", e->f->entry);
                tick(e, out, pc, NULL, count, pending, 1, NULL);
                export_pending(e, out, pending);
                lhg_lines_add(out, "return;");
                lhg_lines_add(out, "}");
                lhg_lines_add(out, "code_epoch = c->code_epoch;");
            }
            const lhg_insn *next = body_at(e, pc);
            if (!next || e->analysis->is_leader[next - e->f->insns]) {
                jump(e, out, pc, count, pending);
                return;
            }
            /* An unknown incoming load affects exactly one instruction.
             * Share the rest of the block with its ordinary entry instead
             * of cloning every instruction up to the next control edge. */
            const lhg_insn *start_insn = body_at(e, start);
            if (e->entries[start_insn - e->f->insns] && count == 1) {
                lhg_lines_addf(out, "ticked -= %d;", count);
                lhg_lines_addf(out, "goto %s;", label(e, pc, pending));
                return;
            }
            continue;
        }

        lhg_lines_addf(out, "c_pc_count(c, %U, 1);", (uint32_t)pc);
        const char *target = op == LHG_OP_JR || op == LHG_OP_JALR ? lhg_fmt(e->arena, "c->r[%d]", (int)i->rs) : lhg_fmt(e->arena, "%U", lhg_insn_target(i));
        lhg_lines_addf(out, "uint32_t target = %s;", target);
        lhg_lines_addf(out, "int take = %s;", condition_text(e, i));
        int link = op == LHG_OP_JALR ? i->rd : (flags & LHG_F_CALL) ? 31 : 0;
        if (e->trace) {
            lhg_lines_addf(out, "c->pc = %U; c_count_recompiled(c, %U, 1);", (uint32_t)pc, (uint32_t)pc);
            lhg_lines_addf(out, "c_retire(c, %d, %U, 0, 0);", link, (uint32_t)(pc + 8));
        } else {
            retire(out, pending, link, lhg_fmt(e->arena, "%U", (uint32_t)(pc + 8)), 0, "0", 0);
        }
        const lhg_insn *slot = body_at(e, pc + 4);
        if (!slot || lhg_insn_flow(slot)) {
            const char *reason = !slot ? "missing branch delay slot" : "branch in branch delay slot (undefined R3000 behavior)";
            observe(e, out, i, 0, count, pc, 1);
            lhg_lines_addf(out, "cpu_stop(c, \"%s\");", reason);
            lhg_lines_add(out, "return;");
            lhg_lines_add(out, "}");
            return;
        }
        lhg_lines code;
        lhg_lines_init(&code, e->arena);
        pending = instruction(e, &code, slot, 0, count + 1, pc);
        if (e->compact && (lhg_op_flags[slot->op] & LHG_F_MEMORY))
            lhg_lines_addf(out, "c->branch_target = take ? target : %U;", (uint32_t)(pc + 8));
        lhg_lines_add(out, "{");
        lhg_lines_addf(out, "c_pc_count(c, %U, 1);", slot->pc);
        lhg_lines_extend(out, &code);
        lhg_lines_add(out, "}");
        count++;
        if ((flags & LHG_F_CALL) && link) {
            lhg_lines_add(out, "if (take) {");
            call(e, out, i, pending, count);
            lhg_lines_add(out, "}");
            jump(e, out, pc + 8, count, pending);
        } else if (op == LHG_OP_JR || op == LHG_OP_JALR) {
            const char *kind = op == LHG_OP_JR && i->rs == 31 ? "C_DISPATCH_RETURN" : "C_DISPATCH_TABLE";
            lhg_lines_addf(out, "c_transfer_stats(c, %U, target, %s);", (uint32_t)pc, kind);
            tick(e, out, 0, "target", count, pending, 1, NULL);
            if (i->rs != 31 || op != LHG_OP_JR) {
                const lhg_jump_table *table = lhg_function_table(e->f, pc);
                if (table) {
                    lhg_lines_add(out, "switch (target) {");
                    for (uint32_t row = 0; row < table->count; row++) {
                        int64_t destination = (int64_t)table->targets[row];
                        if (!body_at(e, destination))
                            continue;
                        lhg_lines_addf(out, "case %U:", (uint32_t)destination);
                        if (!e->compact && !e->trace)
                            tick(e, out, destination, NULL, 0, pending, 1, NULL);
                        lhg_lines_addf(out, "goto %s;", label(e, destination, pending));
                    }
                    lhg_lines_add(out, "default: break;");
                    lhg_lines_add(out, "}");
                }
                export_pending(e, out, pending);
                if (!e->compact)
                    lhg_lines_add(out, "dispatch(c, target);");
            } else {
                export_pending(e, out, pending);
                if (!e->compact)
                    lhg_lines_add(out, "c_return(c);");
            }
            lhg_lines_add(out, "return;");
        } else if (op == LHG_OP_J) {
            lhg_lines_addf(out, "c_transfer_stats(c, %U, target, C_DISPATCH_COUNT);", (uint32_t)pc);
            jump(e, out, (int64_t)lhg_insn_target(i), count, pending);
        } else {
            lhg_lines_add(out, "if (take) {");
            lhg_lines_addf(out, "c_transfer_stats(c, %U, target, C_DISPATCH_COUNT);", (uint32_t)pc);
            jump(e, out, (int64_t)lhg_insn_target(i), count, pending);
            lhg_lines_add(out, "}");
            jump(e, out, pc + 8, count, pending);
        }
        lhg_lines_add(out, "}");
        return;
    }
}

void lhg_emit_fast_variant(lhg_arena *arena, const lhg_analysis *analysis, const lhg_available *available, int trace,
                           int compact, int poll_forward, int direct_memory, lhg_text *out)
{
    const lhg_function *f = analysis->function;
    emitter e;
    memset(&e, 0, sizeof e);
    e.arena = arena;
    e.analysis = analysis;
    e.f = f;
    e.available = available;
    e.trace = trace;
    e.compact = compact;
    e.poll_forward = poll_forward;
    e.direct_memory = direct_memory;
    e.entries = compact ? analysis->compact_entries : analysis->full_entries;
    e.seen = lhg_arena_alloc(arena, (f->insn_count + 1) * sizeof *e.seen);
    memset(e.seen, 0, (f->insn_count + 1) * sizeof *e.seen);

    lhg_lines lines;
    lhg_lines_init(&lines, arena);
    uint32_t entry = f->entry;
    int shared_entry = !compact && !trace;
    lhg_lines_addf(&lines, "void f_%X(cpu_t *c) {", entry);
    lhg_lines_add(&lines, "uint32_t delayed = 0, ticked = 0;");
    lhg_lines_add(&lines, "uint64_t code_epoch = c->code_epoch;");
    if (!compact)
        lhg_lines_addf(&lines, "if (!code_valid(c, &code_%X)) return;", entry);
    lhg_lines_add(&lines, "C_STATS(++c->recomp_functions);");
    int32_t entry_index = (int32_t)(lhg_function_insn(f, (int64_t)entry) - f->insns);
    if (shared_entry) {
        lhg_lines_add(&lines, "uint32_t pc = c->pc, window;");
        lhg_lines_addf(&lines, "if (pc == %U) window = %d;", entry, analysis->windows[entry_index]);
        lhg_lines_add(&lines, "else {");
        lhg_lines_add(&lines, "switch (pc) {");
        for (uint32_t index = 0; index < f->insn_count; index++) {
            if (!e.entries[index] || f->insns[index].pc == entry)
                continue;
            lhg_lines_addf(&lines, "case %U: window = %d; break;", f->insns[index].pc, analysis->windows[index]);
        }
        lhg_lines_add(&lines, "default: return;");
        lhg_lines_add(&lines, "}");
        lhg_lines_add(&lines, "}");
        tick(&e, &lines, 0, "pc", 0, 0, 1, "window");
    }
    if (compact || trace)
        tick(&e, &lines, 0, "c->pc", 0, 0, 1, NULL);
    if (!compact)
        lhg_lines_addf(&lines, "if (c->depth && c->stack[c->depth - 1].pc != %U) c_entry(c, %U);", entry, entry);
    if (!compact) {
        lhg_lines_addf(&lines, "if (%s == %U) {", shared_entry ? "pc" : "c->pc", entry);
        resume(&e, &lines, (int64_t)entry);
        lhg_lines_add(&lines, "}");
    }
    lhg_lines_addf(&lines, "switch (%s) {", shared_entry ? "pc" : "c->pc");
    for (uint32_t index = 0; index < f->insn_count; index++) {
        if (!e.entries[index] || (f->insns[index].pc == entry && !compact))
            continue;
        lhg_lines_addf(&lines, "case %U:", f->insns[index].pc);
        resume(&e, &lines, (int64_t)f->insns[index].pc);
    }
    lhg_lines_add(&lines, "default: return;");
    lhg_lines_add(&lines, "}");
    while (e.todo_head < e.todo_count) {
        block_key key = e.todo[e.todo_head++];
        int64_t pc = (int64_t)f->insns[key.index].pc;
        lhg_lines_addf(&lines, "%s: {", label(&e, pc, key.pending));
        block(&e, &lines, pc, key.pending);
        lhg_lines_add(&lines, "}");
    }
    lhg_lines_add(&lines, "}");
    lhg_lines_add(&lines, "");
    lhg_text_join(out, &lines);
}
