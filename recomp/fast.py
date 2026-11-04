"""Basic-block emission with statically specialized one-instruction load delays.

The pending register is part of a block's translation key, never a runtime tag
in the common path. Only entry from another function/interpreter can have an
unknown tag (-1); that alternate first instruction uses the trace retirement.
"""
from .decode import CALLS
from .emit import effects, operation, u

LOADS = {"lb", "lbu", "lh", "lhu", "lw", "lwl", "lwr", "mfc0", "cfc0", "mfc2", "cfc2"}
STORES = {"sb", "sh", "sw", "swl", "swr"}
OBSERVERS = {"syscall", "gte", "lwc2", "swc2", "mtc2", "ctc2", "break", "reserved", "cop_unusable"}
MEMORY = (LOADS - {"mfc0", "cfc0", "mfc2", "cfc2"}) | STORES | {"lwc2", "swc2"}
DIRECT_MEMORY = {"lb", "lbu", "lh", "lhu", "lw", "sb", "sh", "sw", "lwc2", "swc2"}


def control(i):
    return i.control or i.name.startswith("bc")


def event_windows(function):
    """Remaining instructions to a checkpoint, including forward successors."""
    distance = {}
    for pc, i in sorted(function.instructions.items(), reverse=True):
        if i.name in MEMORY:
            distance[pc] = 1
        elif control(i):
            slot = function.instructions.get(pc + 4)
            targets = [] if i.name in CALLS | {"jr"} or (slot and slot.name in MEMORY) else [i.target]
            if i.name not in {"j", "jr"} and i.name not in CALLS:
                targets.append(pc + 8)
            distance[pc] = 2 + max((distance.get(p, 0) for p in targets if p > pc), default=0)
        else:
            distance[pc] = 1 + distance.get(pc + 4, 0)
    return distance


def event_gap(function):
    """Conservative instruction window between compact runtime checkpoints."""
    return max(event_windows(function).values(), default=1)


class Emitter:
    def __init__(self, function, functions, trace=False, compact=False, poll_forward=False, direct_memory=False):
        self.f = function
        self.functions = functions
        self.body = function.instructions
        self.trace = trace
        self.compact = compact
        self.direct_memory = direct_memory
        self.poll_forward = poll_forward
        self.event_window = event_gap(function)
        self.todo = []
        self.seen = set()
        self.leaders = {function.entry}
        for pc, i in self.body.items():
            if control(i):
                self.leaders.update((i.target, pc + 8))
                self.leaders.update(function.tables.get(pc, []))
        self.entries = {function.entry}
        self.entries.update(self.leaders & self.body.keys())
        if not compact:
            if function.entry + 4 in self.body: self.entries.add(function.entry + 4)
        self.entries.update(pc + 8 for pc, i in self.body.items() if i.name in CALLS and pc + 8 in self.body)
        self.shared_starts = self.leaders | self.entries
        self.windows = {}
        for pc, i in sorted(self.body.items(), reverse=True):
            self.windows[pc] = (2 if control(i) else 0 if i.name in MEMORY else 1 +
                                (self.windows.get(pc + 4, 0) if pc + 4 not in self.leaders else 0))

    def label(self, pc, pending):
        if self.trace: pending = 0
        key = (pc, pending)
        if key not in self.seen:
            self.seen.add(key)
            self.todo.append(key)
        return f"L_{pc:08x}_{'incoming' if pending == -1 else pending}"

    def export(self, pending):
        if self.trace or pending == -1: return []
        return [f"c->load_reg = {pending}; c->load_value = {'delayed' if pending else '0'};"] if pending else []

    def observe(self, i, pending, count, branch=0, advance=True):
        # MMIO sees the cycle immediately before this instruction, not the
        # beginning of the containing block. Timed memory calls receive this
        # static offset explicitly and flush it before touching hardware.
        lines = [f"c->pc = {u(i.pc)};", f"c->in_delay_slot = {int(bool(branch))};"]
        if branch: lines.append(f"c->branch_pc = {u(branch)};")
        lines += self.export(pending)
        if advance and not self.trace:
            lines.append(f"c_count_recompiled(c, {u(self.f.entry)}, {count - 1} - ticked);")
        if advance:
            lines += [f"c_recomp_budget(c, {count - 1} - ticked);",
                  f"c->cycles += (uint64_t)({count - 1} - ticked) * CYCLES_PER_INSN;",
                  f"ticked = {count - 1};"]
        if advance:
            lines.append("c_tick(c, 0);")
        lines += [f"c->fault_instructions = 1; c->fault_counted = {2 if self.trace else 0};"]
        return lines

    def observed(self, pending):
        return ["c->fault_instructions = 0; c->in_delay_slot = 0;"] + (
            ["c->load_reg = c->load_value = 0;"] if pending > 0 and not self.trace else [])

    def retire(self, pending, dst=0, value="0", load=0, loaded="0", defer=False):
        if pending == -1:
            lines = [f"c_retire(c, {dst}, {value}, 0, 0);"]
            # A second load cancels an incoming load to the same register.
            if load:
                lines = [f"c_retire(c, {dst}, {value}, {load}, {loaded});",
                         "delayed = c->load_value; c->load_reg = c->load_value = 0;"]
                if not defer: lines.append(f"c->r[{load}] = delayed;")
            return lines
        lines = []
        if pending and pending not in {dst, load}:
            lines.append(f"c->r[{pending}] = delayed;")
        if dst: lines.append(f"c->r[{dst}] = {value};")
        if load: lines.append(f"{'delayed' if defer else f'c->r[{load}]'} = {loaded};")
        return lines

    def memory(self, i, pending, count, branch):
        n = i.name
        width = {"lb": 1, "lbu": 1, "lh": 2, "lhu": 2, "lw": 4,
                 "sb": 1, "sh": 2, "sw": 4, "lwl": 4, "lwr": 4}[n]
        load = n in LOADS
        partial = n in {"lwl", "lwr"}
        lines = [f"uint32_t address = a + {u(i.signed_imm)};",
                 f"uint32_t aligned = {'address & ~3u' if partial else 'address'};",
                 "uint32_t physical = aligned & 0x1fffffffu;"]
        if load: lines.append("uint32_t memory;")
        window = self.windows.get(i.pc + 4, 0) if not branch else 0
        if self.compact:
            window = f'((c->cop0[12] & 0x401) == 0x401 ? {self.event_window}u : 0u)'
        lines.append(f"int timed = physical >= c->ram_limit || (aligned & {width - 1}u) || "
                     f"(int64_t)(c->cycles + ((uint64_t)({count} - ticked) + {window}) * CYCLES_PER_INSN - c->next_event) >= 0;")
        lines.append("if (!timed) {")
        lines.append(f"memory = ram_read(c, physical, {width});" if load else f"ram_write(c, physical, {width}, b);")
        lines += ["} else {"]
        lines += self.observe(i, pending, count, branch, advance=False)
        lines.append(f"memory = mem_compiled_read(c, aligned, {width}, {count - 1} - ticked);" if load else
                     f"mem_compiled_write(c, aligned, {width}, b, {count - 1} - ticked);")
        lines.append(f"ticked = {count - 1};")
        lines += self.observed(pending) + ["}"]
        cast = {"lb": "(uint32_t)(int32_t)(int8_t)", "lh": "(uint32_t)(int32_t)(int16_t)"}.get(n, "")
        value = cast + "memory"
        if partial:
            old = "delayed" if pending == i.rt and pending > 0 else f"c_merge_value(c, {i.rt})" if pending == -1 else f"c->r[{i.rt}]"
            shift = "(3 - (address & 3)) * 8" if n == "lwl" else "(address & 3) * 8"
            direction = "<<" if n == "lwl" else ">>"
            lines += [f"unsigned shift = {shift};", f"uint32_t mask = 0xffffffffu {direction} shift;"]
            value = f"({old} & ~mask) | (memory {direction} shift)"
        return lines, value

    def direct_instruction(self, i, pending, count, branch):
        n = i.name
        helper = "c_memory_" + n if n in {"lw", "sw"} else "c_memory"
        width = {'lb': 1, 'lbu': 1, 'lh': 2, 'lhu': 2, 'lw': 4,
                 'sb': 1, 'sh': 2, 'sw': 4, 'lwc2': 4, 'swc2': 4}[n]
        load = i.rt if n in LOADS else 0
        window = self.windows.get(i.pc + 4, 0) if not branch else 0
        if self.compact: window = f'((c->cop0[12] & 0x401) == 0x401 ? {self.event_window}u : 0u)'
        lines = [f'uint32_t a = c->r[{i.rs}], b = c->r[{i.rt}];',
                 f'uint32_t address = a + {u(i.signed_imm)};',
                 'uint32_t physical = address & 0x1fffffffu;',
                 f'if (physical < c->ram_limit && !(address & {width - 1}u) && '
                 f'(int64_t)(c->cycles + ((uint64_t)({count} - ticked) + {window}) * CYCLES_PER_INSN - c->next_event) < 0) {{']
        lines = [line.replace('c->r[0]', '0u') for line in lines]
        if n in LOADS:
            cast = {'lb':'(uint32_t)(int32_t)(int8_t)', 'lh':'(uint32_t)(int32_t)(int16_t)'}.get(n,'')
            lines += [f'uint32_t memory = {cast}ram_read(c, physical, {width});']
            lines += self.retire(pending, load=load, loaded='memory', defer=bool(load))
            if not load: lines.append('(void)memory;')
        else:
            if n == 'lwc2': lines.append(f'gte_write(c, {i.rt}, ram_read(c, physical, 4), 0);')
            else:
                value = f'gte_read(c, {i.rt}, 0)' if n == 'swc2' else 'b'
                lines.append(f'ram_write(c, physical, {width}, {value});')
            lines += self.retire(pending)
        lines += ['} else {',
                  f'static const memory_site_t site = {{{u(i.pc)}, {u(branch)}, {count - 1}, {u(i.signed_imm)}, {pending}, {i.rs}, {i.rt}, {i.word >> 26}, {int(bool(load))}, &code_{self.f.entry:08x}}};',
                  f'{"delayed = " if load else ""}{helper}(c, &site, delayed, ticked);',
                  'if (c->memory_exit) return;', 'ticked = c->memory_ticked;', '}']
        return lines, load

    def instruction(self, i, pending, count, successors, branch=0):
        if self.trace:
            # Timing/fault context is block based even in diagnostic mode.
            return self.observe(i, 0, count, branch) + operation(i) + self.observed(0), 0
        n = i.name
        helper = "c_memory_" + n if n in {"lw", "sw"} else "c_memory"
        if n in DIRECT_MEMORY and (not self.compact or self.direct_memory):
            return self.direct_instruction(i, pending, count, branch)
        if self.compact and n in MEMORY and (n not in DIRECT_MEMORY or not self.direct_memory):
            load = i.rt if n in LOADS else 0
            defer = bool(load)
            lines = [f"static const memory_site_t site = {{{u(i.pc)}, {u(branch)}, {count - 1}, {u(i.signed_imm)}, {pending}, {i.rs}, {i.rt}, {i.word >> 26}, {int(defer)}, &code_{self.f.entry:08x}}};",
                     f"{'delayed = ' if defer else ''}{helper}(c, &site, delayed, ticked);",
                     "if (c->memory_exit) return;",
                     "ticked = c->memory_ticked;"]
            return lines, load if defer else 0
        lines, dst, value, load, loaded = effects(i)
        # Reads of r0 are constants; writes are discarded below.
        lines = [line.replace("c->r[0]", "0u") for line in lines]
        if n in (LOADS - {"mfc0", "cfc0", "mfc2", "cfc2"}) | {"sb", "sh", "sw"}:
            lines = lines[:1]
            mem, loaded = self.memory(i, pending, count, branch)
            lines += mem
        elif n in {"add", "addi", "sub"}:
            operand = u(i.signed_imm) if n == "addi" else "b"
            sign = "-" if n == "sub" else "+"
            overflow = f"((a ^ {operand}) & (a ^ value))" if n == "sub" else f"(~(a ^ {operand}) & (a ^ value))"
            lines += [f"uint32_t value = a {sign} {operand};", f"if ({overflow} & 0x80000000u) {{"]
            lines += self.observe(i, pending, count, branch) + ["cpu_exception(c, 12, 0);", "return;", "}"]
            value = "value"
        elif n == "syscall":
            lines += self.retire(pending)
            lines += self.observe(i, 0, count, branch) + ["bios_syscall(c);"] + self.observed(0)
            return lines, 0
        elif n in OBSERVERS | {"mfc2", "cfc2", "swl", "swr"}:
            lines = lines[:1] + self.observe(i, pending, count, branch) + lines[1:]
            lines += [f"uint32_t loaded = {loaded};"] + self.observed(pending)
            loaded = "loaded"
            if n in {"break", "reserved", "cop_unusable"}:
                return lines + ["return;"], 0
        # Capture operands before overwriting either a pending load or its merge.
        if dst and value != "value": lines.append(f"uint32_t value = {value};")
        if dst: value = "value"
        if load and loaded != "loaded": lines.append(f"uint32_t loaded = {loaded};")
        if load: loaded = "loaded"
        # Loads to r0 still perform memory/coprocessor side effects.
        if n in LOADS and not load and loaded not in {"0", "loaded"}: lines.append(f"(void)({loaded});")
        # IRQ handlers can observe a pending load even when the next guest
        # instruction does not read it. Preserve the architectural delay.
        defer = bool(load)
        lines += self.retire(pending, dst, value, load, loaded, defer)
        return lines, load if defer else 0

    def tick(self, pc, count, pending=0, reset=True, window=None):
        if self.compact:
            target = u(pc) if isinstance(pc, int) else pc
            lines = self.export(pending)
            lines += [f"if (!c_block(c, {target}, {count} - ticked, &code_{self.f.entry:08x})) return;",
                      f"ticked = {0 if reset else count};", "code_epoch = c->code_epoch;"]
            if pending > 0: lines.append("c->load_reg = c->load_value = 0;")
            return lines
        lines = [f"c->pc = {u(pc) if isinstance(pc, int) else pc};"]
        lines += self.export(pending)
        if count and not self.trace: lines.append(f"c_count_recompiled(c, {u(self.f.entry)}, {count} - ticked);")
        if count: lines.append(f"c_recomp_budget(c, {count} - ticked);")
        if self.trace:
            lines.append(f"c_tick(c, {count} - ticked); ticked = {0 if reset else count};")
        else:
            if window is None: window = self.windows.get(pc, 0)
            lines += [f"if (!c_tick_window(c, {count} - ticked, {window})) return;",
                      f"ticked = {0 if reset else count};"]
        lines += ["if (code_epoch != c->code_epoch) {",
                  f"if (!code_valid(c, &code_{self.f.entry:08x})) return;",
                  "code_epoch = c->code_epoch;", "}"]
        if pending > 0 and not self.trace: lines.append("c->load_reg = c->load_value = 0;")
        return lines

    def jump(self, target, count, pending):
        if self.compact and not self.poll_forward and target in self.body and target > self.block_start:
            # Carry the unflushed count into a forward successor. Only entry,
            # back edges and external transfers are polling safe points.
            return [f'ticked -= {count};', f'goto {self.label(target, pending)};']
        lines = self.tick(target, count, pending)
        if target in self.body:
            lines.append(f"goto {self.label(target, pending)};")
        else:
            lines += self.export(pending) + ["return;"]
        return lines

    def resume(self, pc):
        if pc not in self.body: return ["return;"]
        if self.trace: return [f"goto {self.label(pc, 0)};"]
        return [f"if (c->load_reg) goto {self.label(pc, -1)};", f"goto {self.label(pc, 0)};"]

    def call(self, i, pending, count):
        back = i.pc + 8
        target = "target" if i.name == "jalr" else u(i.target)
        lines = [f"c_transfer_stats(c, {u(i.pc)}, {target}, C_DISPATCH_CALL);"]
        lines += self.tick(target, count, pending) + self.export(pending)
        if self.compact: return lines + ['return;']
        lines += [f"if (!c_enter(c, {target}, {u(back)})) return;"]
        if i.name != "jalr" and i.target in self.functions:
            lines += [f"f_{i.target:08x}(c);", f"if (c->pc != {u(back)}) dispatch(c, c->pc);"]
        else:
            lines.append(f"dispatch(c, {target});")
        lines += ["c_leave(c);", f"if (c->pc != {u(back)}) return;"]
        return lines + self.tick(back, 0) + self.resume(back)

    def idle_site(self, start):
        """Recognize a bounded RAM countdown around an IRQ-owned clock read."""
        body = [self.body.get(start + i * 4) for i in range(8)]
        if any(i is None for i in body) or body[6].name != 'bne': return None
        body += [self.body.get(body[6].target + i * 4) for i in range(6)]
        if any(i is None for i in body): return None
        a, nop0, sub, store, reload, nop1, timeout, nop2, high, clock, nop3, less, again, nop4 = body
        if [i.name for i in body] != ['lw', 'sll', 'addiu', 'sw', 'lw', 'sll', 'bne', 'sll',
                                     'lui', 'lw', 'sll', 'slt', 'bne', 'sll']:
            return None
        value = a.rt
        if not value or any(i.word for i in (nop0, nop1, nop2, nop3, nop4)): return None
        if not (sub.rs == sub.rt == store.rt == reload.rt == high.rt == clock.rt == clock.rs == less.rd == less.rs == again.rs == value): return None
        if not (a.rs == store.rs == reload.rs and a.imm == store.imm == reload.imm): return None
        if sub.signed_imm != -1 or timeout.rs != value or timeout.target < start + 32: return None
        if again.rt or again.target != start or value in {a.rs, less.rt, timeout.rt}: return None
        return f"{{{u(a.signed_imm)}, {u((high.imm << 16) + clock.signed_imm)}, {a.rs}, {value}, {less.rt}, {timeout.rt}, {u(high.pc)}}}"


    def poll_site(self, start):
        """Prove the battle limiter's eight-instruction, convergent iteration."""
        loop = [self.body.get(start + i * 4) for i in range(4)]
        if any(i is None for i in loop): return None
        call, slot, again, delay = loop
        if call.name != 'jal' or slot.word or delay.word or again.name != 'bne': return None
        if again.target != start or again.rt: return None
        getter = self.functions.get(call.target)
        if not getter or set(getter.instructions) != {call.target + i * 4 for i in range(4)}: return None
        leaf = [getter.instructions[call.target + i * 4] for i in range(4)]
        high, load, ret, nop = leaf
        if [i.name for i in leaf] != ['lui', 'lw', 'jr', 'sll'] or nop.word: return None
        result = high.rt
        if result in (0, 31) or load.rs != result or load.rt != result or again.rs != result or ret.rs != 31: return None
        address = ((high.imm << 16) + load.signed_imm) & 0xffffffff
        physical = address & 0x1fffffff
        if address & 3 or not (physical < 0x800000 or 0x1f800000 <= physical <= 0x1f8003fc): return None
        words = ', '.join(u(i.word) for i in loop + leaf)
        return f'{{{u(start)}, {u(call.target)}, {u(address)}, {result}, {{{words}}}}}'

    def block(self, start, pending):
        self.block_start = start
        pc, count = start, 0
        lines = []
        idle = self.idle_site(start) if pending == 0 and not self.trace else None
        if idle:
            lines += self.tick(start, 0)
            lines += [f"static const idle_site_t idle = {idle};", "c_idle(c, &idle);"]
            lines += self.tick(start, 0)
        poll = self.poll_site(start) if pending == 0 and not self.trace else None
        if poll:
            lines += self.tick(start, 0)
            lines += [f'static const poll_site_t poll = {poll};', 'c_poll_skip(c, &poll);']
            lines += self.tick(start, 0)
        while pc in self.body:
            i = self.body[pc]
            count += 1
            lines.append("{")
            if not control(i):
                lines.append(f"c_pc_count(c, {u(pc)}, 1);")
                code, pending = self.instruction(i, pending, count, [pc + 4])
                lines += code
                memory = {"lb", "lbu", "lh", "lhu", "lw", "lwl", "lwr", "sb", "sh", "sw", "swl", "swr", "lwc2", "swc2"}
                if (i.name in memory and (not self.compact or self.direct_memory and i.name in DIRECT_MEMORY) and (self.trace or i.name not in DIRECT_MEMORY)) or i.name == "syscall":
                    normal = {'lb', 'lbu', 'lh', 'lhu', 'lw', 'lwl', 'lwr', 'sb', 'sh', 'sw'}
                    condition = ('1' if i.name == 'syscall' else 'timed' if i.name in normal and not self.trace else
                                 f"((a + {u(i.signed_imm)}) & 0x1fffffffu) >= c->ram_limit || "
                                 f"(int64_t)(c->cycles + (uint64_t)({count + self.windows.get(pc + 4, 0)} - ticked) * CYCLES_PER_INSN - c->next_event) >= 0")
                    lines += [f"if ({condition}) {{"] + self.tick(pc + 4, count, pending, reset=False) + ["}"]
                lines += ["}"]
                if i.name in {"break", "reserved", "cop_unusable"}: return lines
                pc += 4
                if (i.name in {"mtc0", "ctc0"} and i.rd == 12) or i.name == "rfe":
                    return lines + self.tick(pc, count, pending) + self.resume(pc)
                if i.name in STORES | {"swc2", "syscall"} and (not self.compact or self.direct_memory and i.name in DIRECT_MEMORY):
                    lines += [f"if (code_epoch != c->code_epoch && !code_valid(c, &code_{self.f.entry:08x})) {{"]
                    lines += self.tick(pc, count, pending) + self.export(pending) + ["return;", "}",
                              "code_epoch = c->code_epoch;"]
                if pc in self.leaders or pc not in self.body:
                    return lines + self.jump(pc, count, pending)
                # An unknown incoming load affects exactly one instruction.
                # Share the rest of the block with its ordinary entry instead
                # of cloning every instruction up to the next control edge.
                if start in self.shared_starts and count == 1:
                    return lines + [f'ticked -= {count};', f'goto {self.label(pc, pending)};']
                continue
            n = i.name
            lines.append(f"c_pc_count(c, {u(pc)}, 1);")
            conditions = {
                "beq": f"c->r[{i.rs}] == c->r[{i.rt}]", "bne": f"c->r[{i.rs}] != c->r[{i.rt}]",
                "blez": f"(int32_t)c->r[{i.rs}] <= 0", "bgtz": f"(int32_t)c->r[{i.rs}] > 0",
                "bltz": f"(int32_t)c->r[{i.rs}] < 0", "bgez": f"(int32_t)c->r[{i.rs}] >= 0",
                "bltzal": f"(int32_t)c->r[{i.rs}] < 0", "bgezal": f"(int32_t)c->r[{i.rs}] >= 0",
            }
            target = f"c->r[{i.rs}]" if n in {"jr", "jalr"} else u(i.target)
            cond = conditions.get(n, "0" if n.startswith("bc") and n.endswith("t") else "1")
            lines += [f"uint32_t target = {target};", f"int take = {cond};"]
            link = i.rd if n == "jalr" else 31 if n in CALLS else 0
            if self.trace:
                lines += [f"c->pc = {u(pc)}; c_count_recompiled(c, {u(pc)}, 1);", f"c_retire(c, {link}, {u(pc + 8)}, 0, 0);"]
            else:
                lines += self.retire(pending, link, u(pc + 8))
            slot = self.body.get(pc + 4)
            if slot is None or control(slot):
                reason = "missing branch delay slot" if slot is None else "branch in branch delay slot (undefined R3000 behavior)"
                lines += self.observe(i, 0, count, pc) + [f'cpu_stop(c, "{reason}");', "return;", "}"]
                return lines
            if n in {"jr", "jalr"}: successors = [None]
            elif n in {"j", "jal"}: successors = [i.target]
            else: successors = [i.target, pc + 8]
            code, pending = self.instruction(slot, 0, count + 1, successors, pc)
            if self.compact and slot.name in MEMORY:
                lines.append(f"c->branch_target = take ? target : {u(pc + 8)};")
            lines += ["{", f"c_pc_count(c, {u(slot.pc)}, 1);"] + code + ["}"]
            count += 1
            if n in CALLS and link:
                lines += ["if (take) {"] + self.call(i, pending, count) + ["}"]
                lines += self.jump(pc + 8, count, pending)
            elif n in {"jr", "jalr"}:
                kind = 'C_DISPATCH_RETURN' if n == 'jr' and i.rs == 31 else 'C_DISPATCH_TABLE'
                lines += [f"c_transfer_stats(c, {u(pc)}, target, {kind});"]
                lines += self.tick("target", count, pending)
                if i.rs != 31 or n != "jr":
                    targets = self.f.tables.get(pc, [])
                    if targets:
                        lines.append("switch (target) {")
                        for t in targets:
                            if t in self.body:
                                lines.append(f"case {u(t)}:")
                                if not self.compact and not self.trace: lines += self.tick(t, 0, pending)
                                lines.append(f"goto {self.label(t, pending)};")
                        lines += ["default: break;", "}"]
                    lines += self.export(pending) + ([] if self.compact else ["dispatch(c, target);"])
                else:
                    lines += self.export(pending) + ([] if self.compact else ["c_return(c);"])
                lines.append("return;")
            elif n == "j":
                lines += [f"c_transfer_stats(c, {u(pc)}, target, C_DISPATCH_COUNT);"] + self.jump(i.target, count, pending)
            else:
                lines += ["if (take) {", f"c_transfer_stats(c, {u(pc)}, target, C_DISPATCH_COUNT);"] + self.jump(i.target, count, pending) + ["}"]
                lines += self.jump(pc + 8, count, pending)
            return lines + ["}"]
        return lines

    def emit(self):
        entry = self.f.entry
        shared_entry = not self.compact and not self.trace
        lines = [f"void f_{entry:08x}(cpu_t *c) {{", "uint32_t delayed = 0, ticked = 0;",
                 "uint64_t code_epoch = c->code_epoch;",
                 *([] if self.compact else [f"if (!code_valid(c, &code_{entry:08x})) return;"]),
                 "C_STATS(++c->recomp_functions);"]
        if shared_entry:
            lines += ["uint32_t pc = c->pc, window;",
                      f"if (pc == {u(entry)}) window = {self.windows[entry]};",
                      "else {", "switch (pc) {"]
            for pc in sorted(self.entries - {entry}):
                lines.append(f"case {u(pc)}: window = {self.windows[pc]}; break;")
            lines += ["default: return;", "}", "}"]
            lines += self.tick("pc", 0, window="window")
        if self.compact or self.trace: lines += self.tick("c->pc", 0)
        if not self.compact:
            lines += [f"if (c->depth && c->stack[c->depth - 1].pc != {u(entry)}) c_entry(c, {u(entry)});"]
        if not self.compact:
            lines += [f'if ({"pc" if shared_entry else "c->pc"} == {u(entry)}) {{']
            lines += self.resume(entry) + ['}']
        lines += [f"switch ({'pc' if shared_entry else 'c->pc'}) {{"]
        for pc in sorted(self.entries):
            if pc == entry and not self.compact: continue
            lines += [f"case {u(pc)}:"]
            lines += self.resume(pc)
        lines += ["default: return;", "}"]
        while self.todo:
            pc, pending = self.todo.pop(0)
            label = self.label(pc, pending)
            lines += [f"{label}: {{"] + self.block(pc, pending) + ["}"]
        return "\n".join(lines + ["}", ""])


def emit_fast_function(function, functions, trace=False, poll_forward=False, direct_memory=True, host_precise=False):
    from .precise import emit_precise_function
    fast = emit_precise_function(function) if host_precise and not trace else Emitter(function, functions, trace).emit()
    if trace: return fast
    compact = Emitter(function, functions, compact=True, poll_forward=poll_forward, direct_memory=direct_memory).emit()
    if direct_memory:
        small = Emitter(function, functions, compact=True, poll_forward=poll_forward).emit()
        compact = '#if defined(FF8_COMPACT) || defined(FF8_DIRECT_RAM)\n' + compact + '#else\n' + small + '#endif\n'
    return '#if defined(FF8_COMPACT)\n' + compact + '#else\n' + fast + '#endif\n'
