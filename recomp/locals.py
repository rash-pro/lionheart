"""Bounded GPR-cache lowering for the relaxed play emitter.

The input is the compact emitter's C99, before overlay symbol renaming. Every
CPU-taking helper has an explicit state contract. Unknown helpers fail closed.
No CPU pointer or local-register address escapes from this layer.
"""
from collections import Counter
import json
from pathlib import Path
import re

from .emit import GTE_COMMANDS


GPR = re.compile(r"c->r\[(\d+)\]")
CALL = re.compile(r"\b([a-zA-Z_]\w*)\(c(?=\s*[,\)])")
PARENS = re.compile(r'"(?:\\.|[^"\\])*"|[()]')

# These helpers neither inspect nor mutate GPRs, deliver events, nor unwind.
PRIVATE = {
    "ram_read", "ram_write", "scratch_read", "scratch_write", "c_scratch_fast", "c_div", "c_timing_charge",
    "c_count_recompiled", "c_recomp_budget", "c_pc_count", "c_transfer_stats",
}
PRIVATE.update({"gte_read", "gte_write", "gte_cmd"} | {"gte_" + name for name in GTE_COMMANDS.values()})
VALUE_BOUNDARIES = {"c_block", "c_memory", "c_memory_lw", "c_memory_sw",
                    "c_timing_block", "c_timing_poll", "mem_compiled_read",
                    "c_merge_value", "code_valid"}
VOID_BOUNDARIES = {
    "c_retire", "c_tick", "c_idle", "c_poll_skip", "bios_syscall",
    "cpu_exception", "cpu_stop", "c_set_sr", "c_timing_idle",
    "mem_compiled_write", "c_swl", "c_swr",
}


def read_selection(path):
    selection = json.loads(Path(path).read_text())
    if (not isinstance(selection, dict) or not {"max_registers", "functions"} <= set(selection)
            or set(selection) - {"max_registers", "functions", "fuse_forward"}):
        raise ValueError("register selection needs max_registers and functions")
    cap, functions = selection["max_registers"], selection["functions"]
    if type(cap) is not int or not 1 <= cap <= 8:
        raise ValueError("max_registers must be in 1..8")
    if not isinstance(functions, list) or not functions or any(
        not isinstance(f, str) or not re.fullmatch(r"(?:ov\d+_)?f_[0-9a-f]{8}", f)
        for f in functions
    ) or len(set(functions)) != len(functions):
        raise ValueError("functions must be unique generated function names")
    regions = selection.get("fuse_forward", [])
    if (not isinstance(regions, list) or any(not isinstance(name, str) or name not in functions
                                             for name in regions) or len(set(regions)) != len(regions)):
        raise ValueError("fuse_forward must name unique selected functions")
    return selection


def qualify(function):
    """Computed jumps must have a finite, discovered jump table."""
    return all(i.name not in {"jr", "jalr"} or
               (i.name == "jr" and i.rs == 31) or pc in function.tables
               for pc, i in function.instructions.items())


def cache_registers(source, cap):
    counts = Counter(int(m[1]) for m in GPR.finditer(source) if int(m[1]))
    return tuple(sorted(sorted(counts, key=lambda r: (-counts[r], r))[:cap]))


def lower(source, cap=8):
    registers = cache_registers(source, cap)
    if not registers:
        return source, registers
    source = GPR.sub(lambda m: f"reg_{m[1]}" if int(m[1]) in registers else m[0], source)
    spill = ", ".join(f"c->r[{r}] = reg_{r}" for r in registers)
    reload = ", ".join(f"reg_{r} = c->r[{r}]" for r in registers)
    temporaries = []

    def calls(text):
        result, pos = [], 0
        while match := CALL.search(text, pos):
            result.append(text[pos:match.start()])
            depth, end = 0, None
            for token in PARENS.finditer(text, match.end() - 2):
                if token[0] == "(":
                    depth += 1
                elif token[0] == ")":
                    depth -= 1
                    if depth == 0:
                        end = token.end()
                        break
            if end is None:
                raise ValueError("unclosed generated helper call")
            name = match[1]
            opening = text.index("(", match.start(), match.end())
            call = text[match.start():opening + 1] + calls(text[opening + 1:end - 1]) + ")"
            if name in VALUE_BOUNDARIES:
                temporary = f"regs_result_{len(temporaries)}"
                temporaries.append(temporary)
                call = f"({spill}, {temporary} = {call}, {reload}, {temporary})"
                prefix = text[text.rfind("\n", 0, match.start()) + 1:match.start()]
                if not prefix.strip() and text[end:end + 1] == ";":
                    call = "(void)" + call
            elif name in VOID_BOUNDARIES:
                call = f"({spill}, {call}, {reload}, (void)0)"
            elif name not in PRIVATE:
                raise ValueError(f"unclassified register-cache helper: {name}")
            result.append(call)
            pos = end
        result.append(text[pos:])
        return "".join(result)

    source = calls(source)
    source = source.replace("return;", f"{{ {spill}; return; }}")
    declarations = "\n".join(f"uint32_t reg_{r} = c->r[{r}];" for r in registers)
    if temporaries:
        declarations += "\nuint32_t " + ", ".join(temporaries) + ";"
    opening = source.index("{") + 1
    return source[:opening] + "\n" + declarations + source[opening:], registers
