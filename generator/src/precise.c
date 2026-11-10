/* Static host instructions with the interpreter's retirement/pipeline contract.
 * The address switch selects generated operations, never fetches or decodes an
 * opcode. Calls return to the shared dispatcher after retiring their delay slot. */
#include "emit.h"

static const char *branch_condition(lhg_arena *arena, const lhg_insn *i)
{
    switch ((lhg_op)i->op) {
    case LHG_OP_BEQ: return lhg_fmt(arena, "c->r[%d] == c->r[%d]", (int)i->rs, (int)i->rt);
    case LHG_OP_BNE: return lhg_fmt(arena, "c->r[%d] != c->r[%d]", (int)i->rs, (int)i->rt);
    case LHG_OP_BLEZ: return lhg_fmt(arena, "(int32_t)c->r[%d] <= 0", (int)i->rs);
    case LHG_OP_BGTZ: return lhg_fmt(arena, "(int32_t)c->r[%d] > 0", (int)i->rs);
    case LHG_OP_BLTZ:
    case LHG_OP_BLTZAL: return lhg_fmt(arena, "(int32_t)c->r[%d] < 0", (int)i->rs);
    case LHG_OP_BGEZ:
    case LHG_OP_BGEZAL: return lhg_fmt(arena, "(int32_t)c->r[%d] >= 0", (int)i->rs);
    case LHG_OP_BC0T:
    case LHG_OP_BC2T: return "0";
    default: return "1";
    }
}

void lhg_emit_precise(lhg_arena *arena, const lhg_analysis *analysis, const char *suffix, lhg_text *out)
{
    const lhg_function *f = analysis->function;
    lhg_lines lines;
    lhg_lines_init(&lines, arena);
    lhg_lines_addf(&lines, "void f_%X%s(cpu_t *c) {", f->entry, suffix);
    lhg_lines_addf(&lines, "if (!code_valid(c, &code_%X%s)) return;", f->entry, suffix);
    lhg_lines_add(&lines, "unsigned entered = 0; C_STATS(++c->recomp_functions);");
    lhg_lines_add(&lines, "for (;;) {");
    lhg_lines_add(&lines, "uint32_t next = c->pc + 4, target = 0, back = 0;");
    lhg_lines_add(&lines, "uint32_t old_target = c->branch_target, old_return = c->branch_return;");
    lhg_lines_add(&lines, "unsigned delayed = c->branch_pending, old_kind = c->branch_kind;");
    lhg_lines_add(&lines, "unsigned branch = 0, take = 0, kind = 0;");
    lhg_lines_add(&lines, "uint64_t epoch = c->code_epoch;");
    lhg_lines_add(&lines, "switch (c->pc) {");
    for (uint32_t index = 0; index < f->insn_count; index++) {
        const lhg_insn *i = &f->insns[index];
        uint32_t pc = i->pc;
        lhg_lines_addf(&lines, "case %U: {", pc);
        lhg_lines_addf(&lines, "c_precise_begin(c, %U);", pc);
        if (lhg_insn_flow(i)) {
            const char *condition = branch_condition(arena, i);
            const char *target = i->op == LHG_OP_JR || i->op == LHG_OP_JALR ? lhg_fmt(arena, "c->r[%d]", (int)i->rs) : lhg_fmt(arena, "%U", lhg_insn_target(i));
            int link = i->op == LHG_OP_JALR ? i->rd : (i->op == LHG_OP_JAL || i->op == LHG_OP_BLTZAL || i->op == LHG_OP_BGEZAL) ? 31 : 0;
            const char *kind = link ? "take ? 1u : 0u" : i->op == LHG_OP_JR && i->rs == 31 ? "2u" : "0u";
            int call = i->op == LHG_OP_JAL || i->op == LHG_OP_JALR || i->op == LHG_OP_BLTZAL || i->op == LHG_OP_BGEZAL;
            uint32_t after = (uint32_t)((int64_t)pc + 8);
            lhg_lines_addf(&lines, "branch = 1; take = %s; target = %s; kind = %s;", condition, target, kind);
            lhg_lines_addf(&lines, "if (take) c_transfer_stats(c, %U, target, kind == 1 ? C_DISPATCH_CALL : kind == 2 ? C_DISPATCH_RETURN : %s);", pc,
                           i->op == LHG_OP_JR || i->op == LHG_OP_JALR ? "C_DISPATCH_TABLE" : "C_DISPATCH_COUNT");
            lhg_lines_addf(&lines, "back = %s;", call ? lhg_fmt(arena, "%U", after) : "0");
            lhg_lines_add(&lines, "if (delayed) { cpu_stop(c, \"branch in branch delay slot (undefined R3000 behavior)\"); return; }");
            lhg_lines_addf(&lines, "c_retire(c, %d, %s, 0, 0);", link, link ? lhg_fmt(arena, "%U", after) : "0");
        } else if (i->op == LHG_OP_SYSCALL) {
            lhg_lines_add(&lines, "c_retire(c, 0, 0, 0, 0); bios_syscall(c);");
        } else {
            lhg_effect effect;
            lhg_effects(arena, i, &effect);
            lhg_lines_extend(&lines, &effect.lines);
            lhg_lines_addf(&lines, "uint32_t value = %s;", effect.value);
            lhg_lines_addf(&lines, "uint32_t loaded = %s;", effect.loaded);
            lhg_lines_addf(&lines, "c_retire(c, %d, value, %d, loaded);", effect.dst, effect.load);
        }
        lhg_lines_addf(&lines, "next = %U; break;", (uint32_t)((int64_t)pc + 4));
        lhg_lines_add(&lines, "}");
    }
    lhg_lines_add(&lines, "default: if (!entered) interp_step(c); return;");
    lhg_lines_add(&lines, "}");
    lhg_lines_add(&lines, "if (c->halted) return;");
    lhg_lines_add(&lines, "entered = 1; c->branch_pending = branch;");
    lhg_lines_add(&lines, "if (branch) { c->branch_target = take ? target : next + 4;");
    lhg_lines_add(&lines, "c->branch_pc = c->pc; c->branch_kind = take ? kind : 0; c->branch_return = back; }");
    lhg_lines_add(&lines, "if (delayed) { next = old_target; c->flow = 1; c->flow_kind = old_kind; c->flow_return = old_return; }");
    lhg_lines_add(&lines, "c->pc = next; c->in_delay_slot = 0; c->fault_instructions = 0; c_tick(c, 1);");
    lhg_lines_addf(&lines, "if (c->halted || (delayed && old_kind) || (epoch != c->code_epoch && !code_valid(c, &code_%X%s))) return;", f->entry, suffix);
    lhg_lines_add(&lines, "}");
    lhg_lines_add(&lines, "}");
    lhg_text_join(out, &lines);
    lhg_text_add(out, "\n", 1);
}
