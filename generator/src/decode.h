/* R3000 instruction decoding. Instructions are data; emitters decide their text. */
#ifndef LHG_DECODE_H
#define LHG_DECODE_H

#include <stdint.h>

#define LHG_OPS(X) \
    X(SLL) X(SRL) X(SRA) X(SLLV) X(SRLV) X(SRAV) X(JR) X(JALR) X(SYSCALL) X(BREAK) \
    X(MFHI) X(MTHI) X(MFLO) X(MTLO) X(MULT) X(MULTU) X(DIV) X(DIVU) X(ADD) X(ADDU) \
    X(SUB) X(SUBU) X(AND) X(OR) X(XOR) X(NOR) X(SLT) X(SLTU) X(J) X(JAL) X(BEQ) \
    X(BNE) X(BLEZ) X(BGTZ) X(ADDI) X(ADDIU) X(SLTI) X(SLTIU) X(ANDI) X(ORI) X(XORI) \
    X(LUI) X(LB) X(LH) X(LWL) X(LW) X(LBU) X(LHU) X(LWR) X(SB) X(SH) X(SWL) X(SW) \
    X(SWR) X(LWC2) X(SWC2) X(BLTZ) X(BGEZ) X(BLTZAL) X(BGEZAL) X(MFC0) X(CFC0) X(MTC0) \
    X(CTC0) X(MFC2) X(CFC2) X(MTC2) X(CTC2) X(BC0F) X(BC0T) X(BC2F) X(BC2T) X(GTE) \
    X(RFE) X(RESERVED) X(COP_UNUSABLE)

typedef enum {
#define X(name) LHG_OP_##name,
    LHG_OPS(X)
#undef X
    LHG_OP_COUNT
} lhg_op;

enum {
    LHG_F_BRANCH = 1 << 0,      /* conditional branch set, including bltzal/bgezal */
    LHG_F_CONTROL = 1 << 1,     /* changes flow: branches, j, jal, jr, jalr */
    LHG_F_COPBRANCH = 1 << 2,   /* bc0/bc2 taken-state branches */
    LHG_F_CALL = 1 << 3,        /* jal, jalr, bltzal, bgezal */
    LHG_F_LOAD = 1 << 4,        /* loads and coprocessor reads with a delay slot */
    LHG_F_STORE = 1 << 5,
    LHG_F_MEMORY = 1 << 6,      /* a data-memory access */
    LHG_F_DIRECT_MEMORY = 1 << 7,
    LHG_F_OBSERVER = 1 << 8,    /* instructions that must see current time and state */
    LHG_F_STOP = 1 << 9,        /* break, reserved and cop_unusable end a basic block */
    LHG_F_NORMAL_MEMORY = 1 << 10,
};

/* The packed decoded form; pc alone identifies an instruction inside an image. */
typedef struct {
    uint32_t pc, word;
    uint16_t imm;
    uint8_t op, rs, rt, rd, sh;
} lhg_insn;

extern const uint16_t lhg_op_flags[LHG_OP_COUNT];

lhg_insn lhg_decode(uint32_t word, uint32_t pc);

/* Branch targets are 32-bit; address arithmetic elsewhere stays wider so a
 * pc at the very top of the address space cannot wrap. */
uint32_t lhg_insn_target(const lhg_insn *insn);
int lhg_insn_flow(const lhg_insn *insn);

static inline int32_t lhg_insn_signed_imm(const lhg_insn *insn)
{
    return insn->imm & 0x8000 ? (int32_t)insn->imm - 0x10000 : (int32_t)insn->imm;
}

#endif
