from dataclasses import dataclass


SPECIAL = {
    0: "sll", 2: "srl", 3: "sra", 4: "sllv", 6: "srlv", 7: "srav",
    8: "jr", 9: "jalr", 12: "syscall", 13: "break", 16: "mfhi",
    17: "mthi", 18: "mflo", 19: "mtlo", 24: "mult", 25: "multu",
    26: "div", 27: "divu", 32: "add", 33: "addu", 34: "sub",
    35: "subu", 36: "and", 37: "or", 38: "xor", 39: "nor",
    42: "slt", 43: "sltu",
}
PRIMARY = {
    2: "j", 3: "jal", 4: "beq", 5: "bne", 6: "blez", 7: "bgtz",
    8: "addi", 9: "addiu", 10: "slti", 11: "sltiu", 12: "andi",
    13: "ori", 14: "xori", 15: "lui", 32: "lb", 33: "lh",
    34: "lwl", 35: "lw", 36: "lbu", 37: "lhu", 38: "lwr",
    40: "sb", 41: "sh", 42: "swl", 43: "sw", 46: "swr",
    50: "lwc2", 58: "swc2",
}
BRANCHES = {"beq", "bne", "blez", "bgtz", "bltz", "bgez", "bltzal", "bgezal"}
CALLS = {"jal", "jalr", "bltzal", "bgezal"}


@dataclass(frozen=True)
class Instruction:
    pc: int
    word: int
    name: str
    rs: int
    rt: int
    rd: int
    sh: int
    imm: int

    @property
    def signed_imm(self):
        return self.imm - 0x10000 if self.imm & 0x8000 else self.imm

    @property
    def target(self):
        if self.name in {"j", "jal"}:
            return ((self.pc + 4) & 0xF0000000) | ((self.word & 0x3FFFFFF) << 2)
        return (self.pc + 4 + self.signed_imm * 4) & 0xFFFFFFFF

    @property
    def control(self):
        return self.name in BRANCHES | {"j", "jal", "jr", "jalr"}


def decode(word, pc):
    op, rs, rt = word >> 26, (word >> 21) & 31, (word >> 16) & 31
    rd, sh, fn = (word >> 11) & 31, (word >> 6) & 31, word & 63
    name = PRIMARY.get(op, "reserved")
    if op == 0:
        name = SPECIAL.get(fn, "reserved")
    elif op == 1:
        # On R3000 the unused REGIMM bits alias BLTZ/BGEZ; only 10/11 link.
        name = {16: "bltzal", 17: "bgezal"}.get(rt, "bgez" if rt & 1 else "bltz")
    elif op in (16, 18):
        cop = op - 16
        if rs in (0, 2, 4, 6):
            name = {0: "mfc", 2: "cfc", 4: "mtc", 6: "ctc"}[rs] + str(cop)
        elif rs == 8:
            name = f"bc{cop}{'t' if rt & 1 else 'f'}"
        elif rs >= 16:
            name = "gte" if cop == 2 else ("rfe" if fn == 16 else "reserved")
    elif op in (17, 19, 49, 51, 57, 59):
        name = "cop_unusable"
    return Instruction(pc, word, name, rs, rt, rd, sh, word & 65535)
