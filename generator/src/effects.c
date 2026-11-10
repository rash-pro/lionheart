#include "emit.h"

#include <string.h>

static const char *const gte_names[64] = {
    [0x01] = "rtps", [0x06] = "nclip", [0x0C] = "op", [0x10] = "dpcs", [0x11] = "intpl",
    [0x12] = "mvmva", [0x13] = "ncds", [0x14] = "cdp", [0x16] = "ncdt", [0x1B] = "nccs",
    [0x1C] = "cc", [0x1E] = "ncs", [0x20] = "nct", [0x28] = "sqr", [0x29] = "dcpl",
    [0x2A] = "dpct", [0x2D] = "avsz3", [0x2E] = "avsz4", [0x30] = "rtpt", [0x3D] = "gpf",
    [0x3E] = "gpl", [0x3F] = "ncct",
};

/* Command and option fields are resolved before generating a call site. */
static void gte_command(lhg_arena *arena, lhg_lines *lines, uint32_t word)
{
    unsigned command = word & 63;
    const char *name = gte_names[command];
    if (!name) {
        lhg_lines_addf(lines, "gte_cmd(c, %U);", word);
        return;
    }
    const char *arguments = "c";
    if (command != 6 && command != 45 && command != 46)
        arguments = lhg_fmt(arena, "c, %d, %d", word & (1u << 19) ? 12 : 0, (int)((word >> 10) & 1));
    if (command == 18)
        arguments = lhg_fmt(arena, "%s, %d, %d, %d", arguments, (int)((word >> 17) & 3), (int)((word >> 15) & 3), (int)((word >> 13) & 3));
    lhg_lines_addf(lines, "gte_%s(%s);", name, arguments);
}

/* Reads of FIFO aliases and color/leading-bit registers stay canonical. */
const char *lhg_gte_read_expression(lhg_arena *arena, unsigned reg, int control)
{
    if (control || (reg != 15 && (reg < 28 || reg > 31)))
        return lhg_fmt(arena, "c->gte_%s[%d]", control ? "ctrl" : "data", (int)reg);
    return lhg_fmt(arena, "gte_read(c, %d, %d)", (int)reg, control);
}

static int gte_write_is_special(unsigned reg, int control)
{
    if (control)
        return reg == 4 || reg == 12 || reg == 20 || reg == 26 || reg == 27 || reg == 29 || reg == 30 || reg == 31;
    return reg == 1 || reg == 3 || reg == 5 || (reg >= 7 && reg <= 11) || (reg >= 15 && reg <= 19) || (reg >= 28 && reg <= 31);
}

void lhg_gte_write_statement(lhg_lines *lines, unsigned reg, const char *value, int control)
{
    if (!gte_write_is_special(reg, control))
        lhg_lines_addf(lines, "c->gte_%s[%d] = %s;", control ? "ctrl" : "data", (int)reg, value);
    else
        lhg_lines_addf(lines, "gte_write(c, %d, %s, %d);", (int)reg, value, control);
}

static int memory_width(lhg_op op)
{
    return op == LHG_OP_LB || op == LHG_OP_LBU || op == LHG_OP_SB ? 8 : op == LHG_OP_LH || op == LHG_OP_LHU || op == LHG_OP_SH ? 16 : 32;
}

void lhg_effects(lhg_arena *arena, const lhg_insn *i, lhg_effect *effect)
{
    int rs = i->rs, rt = i->rt, rd = i->rd, sh = i->sh;
    uint32_t imm = (uint32_t)(int32_t)lhg_insn_signed_imm(i);
    lhg_lines *lines = &effect->lines;
    lhg_lines_init(lines, arena);
    lhg_lines_addf(lines, "uint32_t a = c->r[%d], b = c->r[%d];", rs, rt);
    effect->dst = 0;
    effect->value = "0";
    effect->load = 0;
    effect->loaded = "0";

    const char *alu = NULL;
    switch ((lhg_op)i->op) {
    case LHG_OP_SLL: alu = lhg_fmt(arena, "b << %d", sh); break;
    case LHG_OP_SRL: alu = lhg_fmt(arena, "b >> %d", sh); break;
    case LHG_OP_SRA: alu = lhg_fmt(arena, "c_sra(b, %d)", sh); break;
    case LHG_OP_SLLV: alu = "b << (a & 31)"; break;
    case LHG_OP_SRLV: alu = "b >> (a & 31)"; break;
    case LHG_OP_SRAV: alu = "c_sra(b, a)"; break;
    case LHG_OP_ADD: alu = "c_add(c, a, b)"; break;
    case LHG_OP_ADDU: alu = "a + b"; break;
    case LHG_OP_SUB: alu = "c_sub(c, a, b)"; break;
    case LHG_OP_SUBU: alu = "a - b"; break;
    case LHG_OP_AND: alu = "a & b"; break;
    case LHG_OP_OR: alu = "a | b"; break;
    case LHG_OP_XOR: alu = "a ^ b"; break;
    case LHG_OP_NOR: alu = "~(a | b)"; break;
    case LHG_OP_SLT: alu = "(int32_t)a < (int32_t)b"; break;
    case LHG_OP_SLTU: alu = "a < b"; break;
    case LHG_OP_MFHI: alu = "c->hi"; break;
    case LHG_OP_MFLO: alu = "c->lo"; break;
    default: break;
    }
    if (alu) {
        effect->dst = rd;
        effect->value = alu;
        return;
    }
    const char *immediate = NULL;
    switch ((lhg_op)i->op) {
    case LHG_OP_ADDI: immediate = lhg_fmt(arena, "c_add(c, a, %U)", imm); break;
    case LHG_OP_ADDIU: immediate = lhg_fmt(arena, "a + %U", imm); break;
    case LHG_OP_SLTI: immediate = lhg_fmt(arena, "(int32_t)a < (int32_t)%U", imm); break;
    case LHG_OP_SLTIU: immediate = lhg_fmt(arena, "a < %U", imm); break;
    case LHG_OP_ANDI: immediate = lhg_fmt(arena, "a & %U", (uint32_t)i->imm); break;
    case LHG_OP_ORI: immediate = lhg_fmt(arena, "a | %U", (uint32_t)i->imm); break;
    case LHG_OP_XORI: immediate = lhg_fmt(arena, "a ^ %U", (uint32_t)i->imm); break;
    case LHG_OP_LUI: immediate = lhg_fmt(arena, "%U", (uint32_t)i->imm << 16); break;
    default: break;
    }
    if (immediate) {
        effect->dst = rt;
        effect->value = immediate;
        return;
    }
    switch ((lhg_op)i->op) {
    case LHG_OP_MTHI: lhg_lines_add(lines, "c->hi = a;"); break;
    case LHG_OP_MTLO: lhg_lines_add(lines, "c->lo = a;"); break;
    case LHG_OP_MULT:
        lhg_lines_add(lines, "uint64_t product = (uint64_t)((int64_t)(int32_t)a * (int64_t)(int32_t)b);");
        lhg_lines_add(lines, "c->lo = (uint32_t)product; c->hi = (uint32_t)(product >> 32);");
        break;
    case LHG_OP_MULTU:
        lhg_lines_add(lines, "uint64_t product = (uint64_t)a * b;");
        lhg_lines_add(lines, "c->lo = (uint32_t)product; c->hi = (uint32_t)(product >> 32);");
        break;
    case LHG_OP_DIV: lhg_lines_add(lines, "c_div(c, a, b, 1);"); break;
    case LHG_OP_DIVU: lhg_lines_add(lines, "c_div(c, a, b, 0);"); break;
    case LHG_OP_LB:
    case LHG_OP_LBU:
    case LHG_OP_LH:
    case LHG_OP_LHU:
    case LHG_OP_LW: {
        effect->load = rt;
        const char *cast = i->op == LHG_OP_LB ? "(uint32_t)(int32_t)(int8_t)" : i->op == LHG_OP_LH ? "(uint32_t)(int32_t)(int16_t)" : "";
        effect->loaded = lhg_fmt(arena, "%smem_read%d(c, a + %U)", cast, memory_width((lhg_op)i->op), imm);
        break;
    }
    case LHG_OP_LWL:
    case LHG_OP_LWR:
        effect->load = rt;
        effect->loaded = lhg_fmt(arena, "c_%s(c, a + %U, c_merge_value(c, %d))", i->op == LHG_OP_LWL ? "lwl" : "lwr", imm, rt);
        break;
    case LHG_OP_SB:
    case LHG_OP_SH:
    case LHG_OP_SW:
        lhg_lines_addf(lines, "mem_write%d(c, a + %U, b);", memory_width((lhg_op)i->op), imm);
        break;
    case LHG_OP_SWL: lhg_lines_addf(lines, "c_swl(c, a + %U, b);", imm); break;
    case LHG_OP_SWR: lhg_lines_addf(lines, "c_swr(c, a + %U, b);", imm); break;
    case LHG_OP_MFC0:
    case LHG_OP_CFC0:
        effect->load = rt;
        effect->loaded = lhg_fmt(arena, "c->cop0[%d]", rd);
        break;
    case LHG_OP_MTC0:
    case LHG_OP_CTC0:
        if (rd == 12)
            lhg_lines_add(lines, "c_set_sr(c, b);");
        else
            lhg_lines_addf(lines, "c->cop0[%d] = b;", rd);
        break;
    case LHG_OP_RFE: lhg_lines_add(lines, "c_set_sr(c, (c->cop0[12] & ~15u) | ((c->cop0[12] >> 2) & 15));"); break;
    case LHG_OP_MFC2:
    case LHG_OP_CFC2:
        effect->load = rt;
        effect->loaded = lhg_gte_read_expression(arena, (unsigned)rd, i->op == LHG_OP_CFC2);
        break;
    case LHG_OP_MTC2:
    case LHG_OP_CTC2: lhg_gte_write_statement(lines, (unsigned)rd, "b", i->op == LHG_OP_CTC2); break;
    case LHG_OP_LWC2: lhg_lines_addf(lines, "gte_write(c, %d, mem_read32(c, a + %U), 0);", rt, imm); break;
    case LHG_OP_SWC2: lhg_lines_addf(lines, "mem_write32(c, a + %U, gte_read(c, %d, 0));", imm, rt); break;
    case LHG_OP_GTE: gte_command(arena, lines, i->word); break;
    case LHG_OP_SYSCALL: break;
    default:
        lhg_lines_addf(lines, "cpu_exception(c, %d, 0);", i->op == LHG_OP_BREAK ? 9 : i->op == LHG_OP_COP_UNUSABLE ? 11 : 10);
        break;
    }
}

void lhg_operation(lhg_arena *arena, lhg_lines *out, const lhg_insn *i)
{
    lhg_effect effect;
    lhg_effects(arena, i, &effect);
    lhg_lines_addf(out, "c->pc = %U; c_count_recompiled(c, %U, 1);", i->pc, i->pc);
    lhg_lines_extend(out, &effect.lines);
    if (i->op == LHG_OP_SYSCALL) {
        lhg_lines_add(out, "c_retire(c, 0, 0, 0, 0); bios_syscall(c);");
        lhg_lines_add(out, "if (c->halted) return;");
        return;
    }
    /* Evaluate side-effecting memory/overflow expressions before retirement. */
    lhg_lines_addf(out, "uint32_t value = %s;", effect.value);
    lhg_lines_addf(out, "uint32_t loaded = %s;", effect.loaded);
    lhg_lines_addf(out, "c_retire(c, %d, value, %d, loaded);", effect.dst, effect.load);
    lhg_lines_add(out, "if (c->halted) return;");
}
