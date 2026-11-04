import struct
from dataclasses import dataclass, field

from .decode import BRANCHES, decode


@dataclass
class Image:
    data: bytes
    base: int
    entry: int
    gp: int = 0
    sp: int = 0x801FFFF0
    code_ranges: tuple = ()
    entry_points: tuple = ()
    timing_entry_points: tuple = ()

    @classmethod
    def read(cls, path):
        data = path.read_bytes()
        if len(data) < 0x800 or data[:8] != b"PS-X EXE":
            raise ValueError("not a PS-X EXE")
        entry, gp, base, size = struct.unpack_from("<4I", data, 0x10)
        stack, offset = struct.unpack_from("<2I", data, 0x30)
        if size > len(data) - 0x800 or (base & 0x1FFFFFFF) + size > 0x200000:
            raise ValueError("truncated EXE or text outside RAM")
        if base & 3 or size & 3 or entry & 3 or not base <= entry < base + size:
            raise ValueError("unaligned EXE or entry outside text")
        return cls(data[0x800:0x800 + size], base, entry, gp,
                   (stack + offset) & 0xFFFFFFFF if stack else 0x801FFFF0)

    def contains(self, pc):
        return pc % 4 == 0 and self.base <= pc and pc + 4 <= self.base + len(self.data)

    def executable(self, pc):
        return self.contains(pc) and (not self.code_ranges or
                                      any(lo <= pc < hi for lo, hi in self.code_ranges))

    def word(self, pc):
        return struct.unpack_from("<I", self.data, pc - self.base)[0]

    def instruction(self, pc):
        return decode(self.word(pc), pc)


@dataclass
class Function:
    entry: int
    instructions: dict = field(default_factory=dict)
    tables: dict = field(default_factory=dict)
    table_words: set = field(default_factory=set)


def jump_table(image, pc, words=None):
    """Recognize Psy-Q's lui/addiu/addu/lw/nop/jr indexed table idiom."""
    start = max(image.base, pc - 40)
    history = [image.instruction(p) for p in range(start, pc, 4)]
    reg = image.instruction(pc).rs
    load = next((i for i in reversed(history) if i.name == "lw" and i.rt == reg), None)
    if load is None or pc - load.pc > 12:
        return []
    constants = {0: 0}
    bases = {}
    for i in history:
        if i.pc == load.pc:
            base = constants.get(i.rs, bases.get(i.rs))
            if base is None:
                return []
            table = (base + i.signed_imm) & 0xFFFFFFFF
            targets = []
            for index in range(256):
                addr = table + index * 4
                if not image.contains(addr):
                    break
                target = image.word(addr)
                if not image.executable(target) or abs(target - pc) > 0x10000:
                    break
                targets.append(target)
            if len(targets) < 2:
                return []
            if words is not None:
                words.update(table + index * 4 for index in range(len(targets)))
            return sorted(set(targets))
        if i.name == "lui":
            constants[i.rt] = i.imm << 16
        elif i.name in {"addiu", "addi", "ori"} and i.rs in constants:
            constants[i.rt] = ((constants[i.rs] | i.imm) if i.name == "ori"
                               else constants[i.rs] + i.signed_imm) & 0xFFFFFFFF
        elif i.name in {"addu", "add"}:
            known = [constants[r] for r in (i.rs, i.rt) if r in constants]
            if len(known) == 1:
                bases[i.rd] = known[0]
            elif len(known) == 2:
                constants[i.rd] = sum(known) & 0xFFFFFFFF
            else:
                constants.pop(i.rd, None)
        elif i.name in {"sll", "srl", "sra", "or", "subu", "lw"}:
            constants.pop(i.rt if i.name == "lw" else i.rd, None)
    return []


def discover(image, seeds=(), scan_calls=True):
    # Candidate calls are validated against the image; traversal, not a fixed
    # FF8 address range, decides which bytes belong to each function.
    entries = {pc for pc in (image.entry, *image.entry_points, *seeds) if image.executable(pc)}
    for pc in range(image.base, image.base + len(image.data) - 3, 4) if scan_calls else ():
        if not image.executable(pc):
            continue
        i = image.instruction(pc)
        if i.name == "jal" and image.executable(i.target):
            entries.add(i.target)
    pending = list(sorted(entries))
    functions = {}
    while pending:
        entry = pending.pop()
        if entry in functions:
            continue
        f = Function(entry)
        functions[entry] = f
        todo = [entry]
        while todo:
            pc = todo.pop()
            if not image.executable(pc) or pc in f.instructions:
                continue
            if pc != entry and pc in entries:
                continue
            # Do not translate zero-filled buffers as hundreds of thousands of
            # NOPs. A real transfer there remains covered by the interpreter.
            if image.contains(pc + 60) and image.data[pc - image.base:pc - image.base + 64] == bytes(64):
                continue
            i = image.instruction(pc)
            f.instructions[pc] = i
            if i.control or i.name.startswith("bc"):
                if image.executable(pc + 4):
                    f.instructions.setdefault(pc + 4, image.instruction(pc + 4))
                if i.name in {"jal", "bltzal", "bgezal"}:
                    if image.executable(i.target) and i.target not in entries:
                        entries.add(i.target)
                        pending.append(i.target)
                    todo.append(pc + 8)
                elif i.name in BRANCHES or i.name.startswith("bc"):
                    todo.extend([i.target, pc + 8])
                elif i.name == "j":
                    todo.append(i.target)
                elif i.name == "jalr":
                    todo.append(pc + 8)
                elif i.rs != 31:
                    targets = jump_table(image, pc, f.table_words)
                    if targets:
                        f.tables[pc] = targets
                        todo.extend(targets)
            elif i.name not in {"break", "reserved", "cop_unusable"}:
                todo.append(pc + 4)
    # A newly found branch-and-link entry might already be in an older body.
    # Overlap is harmless: each entry has its own reachable translation.
    return {pc: f for pc, f in sorted(functions.items()) if f.instructions}
