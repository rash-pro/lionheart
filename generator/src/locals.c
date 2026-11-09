#include "locals.h"

#include <string.h>

#include "platform.h"

int lhg_cache_contains(const char *const *names, size_t count, const char *name)
{
    for (size_t index = 0; index < count; index++)
        if (names[index] && !strcmp(names[index], name))
            return 1;
    return 0;
}

static int digit(char c)
{
    return c >= '0' && c <= '9';
}

static int word(char c)
{
    return digit(c) || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}

static int space(char c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v';
}

static int function_name(const char *name)
{
    if (!name)
        return 0;
    if (name[0] == 'o' && name[1] == 'v') {
        name += 2;
        if (!digit(*name))
            return 0;
        while (digit(*name))
            name++;
        if (*name++ != '_')
            return 0;
    }
    if (strncmp(name, "f_", 2))
        return 0;
    name += 2;
    for (int index = 0; index < 8; index++) {
        char c = *name++;
        if (!digit(c) && !(c >= 'a' && c <= 'f'))
            return 0;
    }
    return !*name;
}

void lhg_cache_validate(lhg_ctx *ctx, const lhg_register_cache *selection)
{
    if (selection->max_registers < 1 || selection->max_registers > 8)
        lhg_fail(ctx, "max_registers must be in 1..8");
    if (!selection->function_count || !selection->functions)
        lhg_fail(ctx, "functions must be unique generated function names");
    for (size_t index = 0; index < selection->function_count; index++)
        if (!function_name(selection->functions[index]) ||
            lhg_cache_contains(selection->functions, index, selection->functions[index]))
            lhg_fail(ctx, "functions must be unique generated function names");
    if (selection->fuse_forward_count && !selection->fuse_forward)
        lhg_fail(ctx, "fuse_forward must name unique selected functions");
    for (size_t index = 0; index < selection->fuse_forward_count; index++)
        if (!selection->fuse_forward[index] ||
            !lhg_cache_contains(selection->functions, selection->function_count, selection->fuse_forward[index]) ||
            lhg_cache_contains(selection->fuse_forward, index, selection->fuse_forward[index]))
            lhg_fail(ctx, "fuse_forward must name unique selected functions");
}

static const char **selection_names(lhg_arena *arena, const lhg_json *json, const char *error)
{
    if (json->type != LHG_JSON_ARRAY)
        lhg_fail(arena->ctx, "%s", error);
    const char **names = lhg_arena_alloc(arena, (json->as.array.count + 1) * sizeof *names);
    for (size_t index = 0; index < json->as.array.count; index++) {
        const lhg_json *item = json->as.array.items[index];
        if (item->type != LHG_JSON_STRING || strlen(item->as.string.text) != item->as.string.length)
            lhg_fail(arena->ctx, "%s", error);
        names[index] = item->as.string.text;
    }
    return names;
}

const lhg_register_cache *lhg_cache_parse(lhg_arena *arena, const lhg_json *json)
{
    lhg_ctx *ctx = arena->ctx;
    const lhg_json *cap = lhg_json_find(json, "max_registers"), *functions = lhg_json_find(json, "functions");
    if (json->type != LHG_JSON_OBJECT || !cap || !functions)
        lhg_fail(ctx, "register selection needs max_registers and functions");
    for (size_t index = 0; index < json->as.object.count; index++) {
        const lhg_json_member *member = &json->as.object.members[index];
        const char *key = member->key;
        if (strlen(key) != member->key_length ||
            (strcmp(key, "max_registers") && strcmp(key, "functions") && strcmp(key, "fuse_forward")))
            lhg_fail(ctx, "register selection needs max_registers and functions");
    }
    if (cap->type != LHG_JSON_INT || cap->as.integer < 1 || cap->as.integer > 8)
        lhg_fail(ctx, "max_registers must be in 1..8");
    lhg_register_cache *selection = lhg_arena_alloc(arena, sizeof *selection);
    memset(selection, 0, sizeof *selection);
    selection->max_registers = (int)cap->as.integer;
    selection->functions = selection_names(arena, functions, "functions must be unique generated function names");
    selection->function_count = functions->as.array.count;
    const lhg_json *regions = lhg_json_find(json, "fuse_forward");
    if (regions) {
        selection->fuse_forward = selection_names(arena, regions, "fuse_forward must name unique selected functions");
        selection->fuse_forward_count = regions->as.array.count;
    }
    lhg_cache_validate(ctx, selection);
    return selection;
}

const lhg_register_cache *lhg_cache_read(lhg_arena *arena, const char *path)
{
    size_t size;
    const char *data = (const char *)lhg_read_file(arena->ctx, path, &size);
    const lhg_json *json = lhg_json_parse(arena, data, size, path);
    lhg_free(arena->ctx, (void *)data);
    return lhg_cache_parse(arena, json);
}

int lhg_cache_qualify(const lhg_function *f)
{
    for (uint32_t index = 0; index < f->insn_count; index++) {
        const lhg_insn *i = &f->insns[index];
        if ((i->op == LHG_OP_JR || i->op == LHG_OP_JALR) && !(i->op == LHG_OP_JR && i->rs == 31) &&
            !lhg_function_table(f, i->pc))
            return 0;
    }
    return 1;
}

/* Matches c->r[digits] without word boundaries, including inside quoted text,
 * just as the textual lowering does. Emitted register indices are in 0..31. */
static size_t gpr(const char *text, const char *end, int *reg)
{
    if (end - text < 7 || memcmp(text, "c->r[", 5) || !digit(text[5]))
        return 0;
    const char *at = text + 5;
    unsigned value = 0;
    while (at < end && digit(*at)) {
        value = value < 32 ? value * 10 + (unsigned)(*at - '0') : 32;
        at++;
    }
    if (at == end || *at != ']' || value >= 32)
        return 0;
    *reg = (int)value;
    return (size_t)(at + 1 - text);
}

typedef struct {
    lhg_arena *arena;
    const char *spill, *reload;
    size_t temporaries;
} lowering;

static int helper_kind(const char *name)
{
    static const char *const values[] = {
        "c_block", "c_memory", "c_memory_lw", "c_memory_sw", "c_timing_block", "c_timing_poll",
        "mem_compiled_read", "c_merge_value", "code_valid"
    };
    static const char *const voids[] = {
        "c_retire", "c_tick", "c_idle", "c_poll_skip", "bios_syscall", "cpu_exception", "cpu_stop", "c_set_sr",
        "c_timing_idle", "mem_compiled_write", "c_swl", "c_swr"
    };
    static const char *const private_helpers[] = {
        "ram_read", "ram_write", "scratch_read", "scratch_write", "c_scratch_fast", "c_div", "c_timing_charge",
        "c_count_recompiled", "c_recomp_budget", "c_pc_count", "c_transfer_stats", "gte_read", "gte_write", "gte_cmd",
        "gte_rtps", "gte_nclip", "gte_op", "gte_dpcs", "gte_intpl", "gte_mvmva", "gte_ncds", "gte_cdp", "gte_ncdt",
        "gte_nccs", "gte_cc", "gte_ncs", "gte_nct", "gte_sqr", "gte_dcpl", "gte_dpct", "gte_avsz3", "gte_avsz4",
        "gte_rtpt", "gte_gpf", "gte_gpl", "gte_ncct"
    };
    if (lhg_cache_contains(values, sizeof values / sizeof *values, name))
        return 1;
    if (lhg_cache_contains(voids, sizeof voids / sizeof *voids, name))
        return 2;
    if (lhg_cache_contains(private_helpers, sizeof private_helpers / sizeof *private_helpers, name))
        return 3;
    return 0;
}

/* Inner calls allocate their temporaries first; their order reaches the
 * declarations even when an outer boundary spills the same registers. */
static void lower_calls(lowering *lower, const char *text, const char *end, lhg_text *out)
{
    const char *pos = text, *at = text;
    while (at < end) {
        if ((!word(*at) || digit(*at)) || (at > text && word(at[-1]))) {
            at++;
            continue;
        }
        const char *opening = at + 1;
        while (opening < end && word(*opening))
            opening++;
        if (end - opening < 3 || opening[0] != '(' || opening[1] != 'c') {
            at = opening;
            continue;
        }
        const char *after_c = opening + 2;
        while (after_c < end && space(*after_c))
            after_c++;
        if (after_c == end || (*after_c != ',' && *after_c != ')')) {
            at = opening;
            continue;
        }
        const char *close = opening + 1;
        unsigned depth = 1;
        for (; close < end; close++) {
            if (*close == '"') {
                const char *quote = close + 1;
                while (quote < end && *quote != '"') {
                    if (*quote == '\\' && quote + 1 < end)
                        quote++;
                    quote++;
                }
                if (quote < end) {
                    close = quote;
                    continue;
                }
            }
            if (*close == '(')
                depth++;
            else if (*close == ')' && !--depth)
                break;
        }
        if (close == end)
            lhg_fail(out->ctx, "unclosed generated helper call");
        lhg_text_add(out, pos, (size_t)(at - pos));
        lhg_text call;
        lhg_text_init(&call, out->ctx);
        lhg_text_add(&call, at, (size_t)(opening + 1 - at));
        lower_calls(lower, opening + 1, close, &call);
        lhg_text_adds(&call, ")");
        const char *name = lhg_arena_strdup(lower->arena, at, (size_t)(opening - at));
        int kind = helper_kind(name);
        const char *call_text = lhg_arena_strdup(lower->arena, call.data, call.size);
        if (kind == 1) {
            size_t temporary = lower->temporaries++;
            const char *prefix = at;
            while (prefix > text && prefix[-1] != '\n')
                prefix--;
            while (prefix < at && space(*prefix))
                prefix++;
            if (prefix == at && close + 1 < end && close[1] == ';')
                lhg_text_adds(out, "(void)");
            lhg_text_addf(out, lower->arena, "(%s, regs_result_%D = %s, %s, regs_result_%D)", lower->spill,
                          (int64_t)temporary, call_text, lower->reload, (int64_t)temporary);
        } else if (kind == 2) {
            lhg_text_addf(out, lower->arena, "(%s, %s, %s, (void)0)", lower->spill, call_text, lower->reload);
        } else if (kind == 3) {
            lhg_text_add(out, call.data, call.size);
        } else {
            lhg_fail(out->ctx, "unclassified register-cache helper: %s", name);
        }
        lhg_text_free(&call);
        at = pos = close + 1;
    }
    lhg_text_add(out, pos, (size_t)(end - pos));
}

void lhg_cache_lower(lhg_arena *arena, const lhg_text *source, int cap, lhg_text *out)
{
    if (!source->size)
        return;
    size_t counts[32] = {0};
    uint8_t selected[32] = {0};
    const char *end = source->data + source->size;
    for (const char *at = source->data; at < end;) {
        int reg;
        size_t size = gpr(at, end, &reg);
        if (size && reg)
            counts[reg]++;
        at += size ? size : 1;
    }
    int used = 0;
    for (; used < cap; used++) {
        int best = 0;
        for (int reg = 1; reg < 32; reg++)
            if (!selected[reg] && counts[reg] > counts[best])
                best = reg;
        if (!best)
            break;
        selected[best] = 1;
    }
    if (!used) {
        lhg_text_add(out, source->data, source->size);
        return;
    }
    lhg_text rewritten, calls, spill, reload;
    lhg_text_init(&rewritten, out->ctx);
    lhg_text_init(&calls, out->ctx);
    lhg_text_init(&spill, out->ctx);
    lhg_text_init(&reload, out->ctx);
    for (int reg = 1; reg < 32; reg++) {
        if (!selected[reg])
            continue;
        if (spill.size) {
            lhg_text_adds(&spill, ", ");
            lhg_text_adds(&reload, ", ");
        }
        lhg_text_addf(&spill, arena, "c->r[%d] = reg_%d", reg, reg);
        lhg_text_addf(&reload, arena, "reg_%d = c->r[%d]", reg, reg);
    }
    for (const char *at = source->data; at < end;) {
        int reg;
        size_t size = gpr(at, end, &reg);
        if (size && selected[reg]) {
            lhg_text_adds(&rewritten, "reg_");
            lhg_text_add(&rewritten, at + 5, size - 6);
        } else {
            lhg_text_add(&rewritten, at, size ? size : 1);
        }
        at += size ? size : 1;
    }
    lowering lower = {arena, lhg_arena_strdup(arena, spill.data, spill.size), lhg_arena_strdup(arena, reload.data, reload.size), 0};
    lower_calls(&lower, rewritten.data, rewritten.data + rewritten.size, &calls);
    const char *body = lhg_arena_strdup(arena, calls.data, calls.size);
    const char *opening = strchr(body, '{');
    if (!opening)
        lhg_fail(out->ctx, "missing generated function body");
    lhg_text_add(out, body, (size_t)(opening + 1 - body));
    for (int reg = 1; reg < 32; reg++)
        if (selected[reg])
            lhg_text_addf(out, arena, "\nuint32_t reg_%d = c->r[%d];", reg, reg);
    if (lower.temporaries) {
        lhg_text_adds(out, "\nuint32_t ");
        for (size_t index = 0; index < lower.temporaries; index++)
            lhg_text_addf(out, arena, "%sregs_result_%D", index ? ", " : "", (int64_t)index);
        lhg_text_adds(out, ";");
    }
    /* Return replacement follows helper rewriting and also visits quoted text. */
    const char *pos = opening + 1, *hit;
    while ((hit = strstr(pos, "return;"))) {
        lhg_text_add(out, pos, (size_t)(hit - pos));
        lhg_text_addf(out, arena, "{ %s; return; }", lower.spill);
        pos = hit + 7;
    }
    lhg_text_adds(out, pos);
    lhg_text_free(&rewritten);
    lhg_text_free(&calls);
    lhg_text_free(&spill);
    lhg_text_free(&reload);
}
