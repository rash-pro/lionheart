"""Static host instructions with the interpreter's retirement/pipeline contract.

The address switch selects generated operations, never fetches or decodes an
opcode. Calls return to the shared dispatcher after retiring their delay slot.
"""
from .emit import effects, u


def emit_precise_function(function):
    entry = function.entry
    lines = [f"void f_{entry:08x}(cpu_t *c) {{",
             f"if (!code_valid(c, &code_{entry:08x})) return;",
             "unsigned entered = 0; C_STATS(++c->recomp_functions);",
             "for (;;) {",
             "uint32_t next = c->pc + 4, target = 0, back = 0;",
             "uint32_t old_target = c->branch_target, old_return = c->branch_return;",
             "unsigned delayed = c->branch_pending, old_kind = c->branch_kind;",
             "unsigned branch = 0, take = 0, kind = 0;",
             "uint64_t epoch = c->code_epoch;",
             "switch (c->pc) {"]
    conditions = {
        "beq": lambda i: f"c->r[{i.rs}] == c->r[{i.rt}]",
        "bne": lambda i: f"c->r[{i.rs}] != c->r[{i.rt}]",
        "blez": lambda i: f"(int32_t)c->r[{i.rs}] <= 0",
        "bgtz": lambda i: f"(int32_t)c->r[{i.rs}] > 0",
        "bltz": lambda i: f"(int32_t)c->r[{i.rs}] < 0",
        "bgez": lambda i: f"(int32_t)c->r[{i.rs}] >= 0",
        "bltzal": lambda i: f"(int32_t)c->r[{i.rs}] < 0",
        "bgezal": lambda i: f"(int32_t)c->r[{i.rs}] >= 0",
    }
    for pc, i in sorted(function.instructions.items()):
        lines += [f"case {u(pc)}: {{", f"c_precise_begin(c, {u(pc)});"]
        if i.control or i.name.startswith("bc"):
            cond = conditions[i.name](i) if i.name in conditions else "0" if i.name.startswith("bc") and i.name.endswith("t") else "1"
            target = f"c->r[{i.rs}]" if i.name in ("jr", "jalr") else u(i.target)
            link = i.rd if i.name == "jalr" else 31 if i.name in ("jal", "bltzal", "bgezal") else 0
            kind = "take ? 1u : 0u" if link else "2u" if i.name == "jr" and i.rs == 31 else "0u"
            lines += [f"branch = 1; take = {cond}; target = {target}; kind = {kind};",
                      f"if (take) c_transfer_stats(c, {u(pc)}, target, kind == 1 ? C_DISPATCH_CALL : kind == 2 ? C_DISPATCH_RETURN : {'C_DISPATCH_TABLE' if i.name in ('jr', 'jalr') else 'C_DISPATCH_COUNT'});",
                      f"back = {u(pc + 8) if i.name in ('jal', 'jalr', 'bltzal', 'bgezal') else '0'};",
                      'if (delayed) { cpu_stop(c, "branch in branch delay slot (undefined R3000 behavior)"); return; }',
                      f"c_retire(c, {link}, {u(pc + 8) if link else '0'}, 0, 0);"]
        elif i.name == "syscall":
            lines += ["c_retire(c, 0, 0, 0, 0); bios_syscall(c);"]
        else:
            body, dst, value, load, loaded = effects(i)
            lines += body + [f"uint32_t value = {value};", f"uint32_t loaded = {loaded};",
                             f"c_retire(c, {dst}, value, {load}, loaded);"]
        lines += [f"next = {u(pc + 4)}; break;", "}"]
    lines += ["default: if (!entered) interp_step(c); return;", "}",
              "if (c->halted) return;",
              "entered = 1; c->branch_pending = branch;",
              "if (branch) { c->branch_target = take ? target : next + 4;",
              "c->branch_pc = c->pc; c->branch_kind = take ? kind : 0; c->branch_return = back; }",
              "if (delayed) { next = old_target; c->flow = 1; c->flow_kind = old_kind; c->flow_return = old_return; }",
              "c->pc = next; c->in_delay_slot = 0; c->fault_instructions = 0; c_tick(c, 1);",
              f"if (c->halted || (delayed && old_kind) || (epoch != c->code_epoch && !code_valid(c, &code_{entry:08x}))) return;",
              "}", "}"]
    return "\n".join(lines) + "\n"
