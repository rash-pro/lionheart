#include "decode.h"

#define F_BRANCH LHG_F_BRANCH
#define F_CONTROL LHG_F_CONTROL
#define F_COP LHG_F_COPBRANCH
#define F_CALL LHG_F_CALL
#define F_LOAD LHG_F_LOAD
#define F_STORE LHG_F_STORE
#define F_MEMORY LHG_F_MEMORY
#define F_DIRECT LHG_F_DIRECT_MEMORY
#define F_OBSERVER LHG_F_OBSERVER
#define F_STOP LHG_F_STOP
#define F_NORMAL LHG_F_NORMAL_MEMORY

const uint16_t lhg_op_flags[LHG_OP_COUNT] = {
    [LHG_OP_JR] = F_CONTROL,
    [LHG_OP_JALR] = F_CONTROL | F_CALL,
    [LHG_OP_J] = F_CONTROL,
    [LHG_OP_JAL] = F_CONTROL | F_CALL,
    [LHG_OP_BEQ] = F_CONTROL | F_BRANCH,
    [LHG_OP_BNE] = F_CONTROL | F_BRANCH,
    [LHG_OP_BLEZ] = F_CONTROL | F_BRANCH,
    [LHG_OP_BGTZ] = F_CONTROL | F_BRANCH,
    [LHG_OP_BLTZ] = F_CONTROL | F_BRANCH,
    [LHG_OP_BGEZ] = F_CONTROL | F_BRANCH,
    [LHG_OP_BLTZAL] = F_CONTROL | F_BRANCH | F_CALL,
    [LHG_OP_BGEZAL] = F_CONTROL | F_BRANCH | F_CALL,
    [LHG_OP_BC0F] = F_COP,
    [LHG_OP_BC0T] = F_COP,
    [LHG_OP_BC2F] = F_COP,
    [LHG_OP_BC2T] = F_COP,
    [LHG_OP_LB] = F_LOAD | F_MEMORY | F_DIRECT | F_NORMAL,
    [LHG_OP_LBU] = F_LOAD | F_MEMORY | F_DIRECT | F_NORMAL,
    [LHG_OP_LH] = F_LOAD | F_MEMORY | F_DIRECT | F_NORMAL,
    [LHG_OP_LHU] = F_LOAD | F_MEMORY | F_DIRECT | F_NORMAL,
    [LHG_OP_LW] = F_LOAD | F_MEMORY | F_DIRECT | F_NORMAL,
    [LHG_OP_LWL] = F_LOAD | F_MEMORY | F_NORMAL,
    [LHG_OP_LWR] = F_LOAD | F_MEMORY | F_NORMAL,
    [LHG_OP_MFC0] = F_LOAD,
    [LHG_OP_CFC0] = F_LOAD,
    [LHG_OP_MFC2] = F_LOAD,
    [LHG_OP_CFC2] = F_LOAD,
    [LHG_OP_SB] = F_STORE | F_MEMORY | F_DIRECT | F_NORMAL,
    [LHG_OP_SH] = F_STORE | F_MEMORY | F_DIRECT | F_NORMAL,
    [LHG_OP_SW] = F_STORE | F_MEMORY | F_DIRECT | F_NORMAL,
    [LHG_OP_SWL] = F_STORE | F_MEMORY,
    [LHG_OP_SWR] = F_STORE | F_MEMORY,
    [LHG_OP_LWC2] = F_MEMORY | F_DIRECT | F_OBSERVER,
    [LHG_OP_SWC2] = F_MEMORY | F_DIRECT | F_OBSERVER,
    [LHG_OP_SYSCALL] = F_OBSERVER,
    [LHG_OP_GTE] = F_OBSERVER,
    [LHG_OP_MTC2] = F_OBSERVER,
    [LHG_OP_CTC2] = F_OBSERVER,
    [LHG_OP_BREAK] = F_OBSERVER | F_STOP,
    [LHG_OP_RESERVED] = F_OBSERVER | F_STOP,
    [LHG_OP_COP_UNUSABLE] = F_OBSERVER | F_STOP,
};

static const uint8_t special_ops[64] = {
    [0] = LHG_OP_SLL + 1, [2] = LHG_OP_SRL + 1, [3] = LHG_OP_SRA + 1, [4] = LHG_OP_SLLV + 1,
    [6] = LHG_OP_SRLV + 1, [7] = LHG_OP_SRAV + 1, [8] = LHG_OP_JR + 1, [9] = LHG_OP_JALR + 1,
    [12] = LHG_OP_SYSCALL + 1, [13] = LHG_OP_BREAK + 1, [16] = LHG_OP_MFHI + 1, [17] = LHG_OP_MTHI + 1,
    [18] = LHG_OP_MFLO + 1, [19] = LHG_OP_MTLO + 1, [24] = LHG_OP_MULT + 1, [25] = LHG_OP_MULTU + 1,
    [26] = LHG_OP_DIV + 1, [27] = LHG_OP_DIVU + 1, [32] = LHG_OP_ADD + 1, [33] = LHG_OP_ADDU + 1,
    [34] = LHG_OP_SUB + 1, [35] = LHG_OP_SUBU + 1, [36] = LHG_OP_AND + 1, [37] = LHG_OP_OR + 1,
    [38] = LHG_OP_XOR + 1, [39] = LHG_OP_NOR + 1, [42] = LHG_OP_SLT + 1, [43] = LHG_OP_SLTU + 1,
};

static const uint8_t primary_ops[64] = {
    [2] = LHG_OP_J + 1, [3] = LHG_OP_JAL + 1, [4] = LHG_OP_BEQ + 1, [5] = LHG_OP_BNE + 1,
    [6] = LHG_OP_BLEZ + 1, [7] = LHG_OP_BGTZ + 1, [8] = LHG_OP_ADDI + 1, [9] = LHG_OP_ADDIU + 1,
    [10] = LHG_OP_SLTI + 1, [11] = LHG_OP_SLTIU + 1, [12] = LHG_OP_ANDI + 1, [13] = LHG_OP_ORI + 1,
    [14] = LHG_OP_XORI + 1, [15] = LHG_OP_LUI + 1, [32] = LHG_OP_LB + 1, [33] = LHG_OP_LH + 1,
    [34] = LHG_OP_LWL + 1, [35] = LHG_OP_LW + 1, [36] = LHG_OP_LBU + 1, [37] = LHG_OP_LHU + 1,
    [38] = LHG_OP_LWR + 1, [40] = LHG_OP_SB + 1, [41] = LHG_OP_SH + 1, [42] = LHG_OP_SWL + 1,
    [43] = LHG_OP_SW + 1, [46] = LHG_OP_SWR + 1, [50] = LHG_OP_LWC2 + 1, [58] = LHG_OP_SWC2 + 1,
};

lhg_insn lhg_decode(uint32_t word, uint32_t pc)
{
    unsigned op = word >> 26, rs = (word >> 21) & 31, rt = (word >> 16) & 31;
    unsigned rd = (word >> 11) & 31, sh = (word >> 6) & 31, fn = word & 63;
    int name = primary_ops[op] ? primary_ops[op] - 1 : LHG_OP_RESERVED;
    if (op == 0) {
        name = special_ops[fn] ? special_ops[fn] - 1 : LHG_OP_RESERVED;
    } else if (op == 1) {
        /* On R3000 the unused REGIMM bits alias BLTZ/BGEZ; only 10/11 link. */
        if (rt == 16)
            name = LHG_OP_BLTZAL;
        else if (rt == 17)
            name = LHG_OP_BGEZAL;
        else
            name = rt & 1 ? LHG_OP_BGEZ : LHG_OP_BLTZ;
    } else if (op == 16 || op == 18) {
        int gte = op == 18;
        if (rs == 0 || rs == 2 || rs == 4 || rs == 6) {
            static const uint8_t table[2][4] = {
                {LHG_OP_MFC0, LHG_OP_CFC0, LHG_OP_MTC0, LHG_OP_CTC0},
                {LHG_OP_MFC2, LHG_OP_CFC2, LHG_OP_MTC2, LHG_OP_CTC2},
            };
            name = table[gte][rs / 2];
        } else if (rs == 8) {
            name = gte ? (rt & 1 ? LHG_OP_BC2T : LHG_OP_BC2F) : (rt & 1 ? LHG_OP_BC0T : LHG_OP_BC0F);
        } else if (rs >= 16) {
            name = gte ? LHG_OP_GTE : (fn == 16 ? LHG_OP_RFE : LHG_OP_RESERVED);
        }
    } else if (op == 17 || op == 19 || op == 49 || op == 51 || op == 57 || op == 59) {
        name = LHG_OP_COP_UNUSABLE;
    }
    lhg_insn insn;
    insn.pc = pc;
    insn.word = word;
    insn.imm = (uint16_t)(word & 65535);
    insn.op = (uint8_t)name;
    insn.rs = (uint8_t)rs;
    insn.rt = (uint8_t)rt;
    insn.rd = (uint8_t)rd;
    insn.sh = (uint8_t)sh;
    return insn;
}

uint32_t lhg_insn_target(const lhg_insn *insn)
{
    if (insn->op == LHG_OP_J || insn->op == LHG_OP_JAL) {
        uint64_t next = (uint64_t)insn->pc + 4;
        return (uint32_t)((next & 0xF0000000u) | ((uint64_t)(insn->word & 0x3FFFFFFu) << 2));
    }
    return (uint32_t)((int64_t)insn->pc + 4 + (int64_t)lhg_insn_signed_imm(insn) * 4);
}

int lhg_insn_flow(const lhg_insn *insn)
{
    return (lhg_op_flags[insn->op] & (LHG_F_CONTROL | LHG_F_COPBRANCH)) != 0;
}
