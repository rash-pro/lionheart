"""Opt-in timing experiment. The exact emitter never imports this policy."""
from .emit import u, gte_read_expression, gte_write_statement
from .fast import Emitter, LOADS, MEMORY, control


class TimingEmitter(Emitter):
    def __init__(self, function, functions, loose=False, inline_blocks=True, fuse_forward=False):
        if fuse_forward and len(function.instructions) > 512:
            raise ValueError(f"forward region at 0x{function.entry:08x} exceeds 512 instructions")
        super().__init__(function, functions, compact=True, poll_forward=not fuse_forward, direct_memory=True)
        self.loose = loose
        self.inline_blocks = inline_blocks

    def observe(self, i, pending, count, branch=0, advance=True):
        # GTE and fault context need a canonical PC, but do not run the device
        # calendar in the middle of a block. MMIO flushes its prefix explicitly.
        if i.name == 'syscall':
            return super().observe(i, pending, count, branch, advance)
        fault = control(i) or i.name in {'add', 'addi', 'sub', 'break', 'reserved', 'cop_unusable'}
        lines = super().observe(i, pending, count, branch, advance=advance and fault)
        return [line for line in lines if line != 'c_tick(c, 0);']

    def tick(self, pc, count, pending=0, reset=True, window=None, inline=True):
        target = u(pc) if isinstance(pc, int) else pc
        lines = self.export(pending)
        mask = 255 if self.loose else 0
        boundary = [f'if (!c_timing_block(c, {target}, {count} - ticked, &code_{self.f.entry:08x})) return;']
        if inline and self.inline_blocks:
            lines += ['#ifdef FF8_TIMING_INLINE',
                  f'if (!c_timing_charge(c, {target}, {count} - ticked, &code_{self.f.entry:08x}, code_epoch, {mask}u)) {{',
                  f'if (!c_timing_poll(c, &code_{self.f.entry:08x}, {mask}u)) return;', '}', '#else',
                  f'if (!c_timing_block(c, {target}, {count} - ticked, &code_{self.f.entry:08x})) return;', '#endif',
                  ]
        else:
            lines += boundary
        lines += [f'ticked = {0 if reset else count};', 'code_epoch = c->code_epoch;']
        if pending > 0:
            lines.append('c->load_reg = c->load_value = 0;')
        return lines

    def instruction(self, i, pending, count, successors, branch=0):
        if i.name not in MEMORY:
            return super().instruction(i, pending, count, successors, branch)
        n = i.name
        load = i.rt if n in LOADS else 0
        partial = n in {'lwl', 'lwr', 'swl', 'swr'}
        width = {'lb': 1, 'lbu': 1, 'sb': 1, 'lh': 2, 'lhu': 2, 'sh': 2}.get(n, 4)
        lines = [f'uint32_t a = c->r[{i.rs}], b = c->r[{i.rt}];',
                 f'uint32_t address = a + {u(i.signed_imm)};',
                 f'uint32_t aligned = {"address & ~3u" if partial else "address"};',
                 'uint32_t physical = aligned & 0x1fffffffu;',
                 f'int scratch = c_scratch_fast(c, physical, {width});',
                 f'int timed = (physical >= c->ram_limit && !scratch) || (aligned & {width - 1}u);']
        lines = [line.replace('c->r[0]', '0u') for line in lines]
        if n in LOADS or n == 'lwc2':
            lines += ['uint32_t memory;', 'if (!timed) {',
                      f'memory = scratch ? scratch_read(c, physical, {width}) : ram_read(c, physical, {width});',
                      '} else {']
            lines += self.observe(i, pending, count, branch, advance=False)
            lines += [f'memory = mem_compiled_read(c, aligned, {width}, {count - 1} - ticked);',
                      f'ticked = {count - 1};'] + self.observed(pending) + ['}']
            if n == 'lwc2':
                lines += [gte_write_statement(i.rt, 'memory', False)] + self.retire(pending)
            else:
                value = {'lb': '(uint32_t)(int32_t)(int8_t)memory',
                         'lh': '(uint32_t)(int32_t)(int16_t)memory'}.get(n, 'memory')
                if partial:
                    old = 'delayed' if pending == i.rt and pending > 0 else f'c_merge_value(c, {i.rt})' if pending == -1 else f'c->r[{i.rt}]'
                    shift = '(3 - (address & 3)) * 8' if n == 'lwl' else '(address & 3) * 8'
                    direction = '<<' if n == 'lwl' else '>>'
                    lines += [f'unsigned shift = {shift};', f'uint32_t mask = 0xffffffffu {direction} shift;']
                    value = f'({old} & ~mask) | (memory {direction} shift)'
                lines += [f'uint32_t loaded = {value};']
                lines += self.retire(pending, load=load, loaded='loaded', defer=bool(load))
                if not load: lines.append('(void)loaded;')
        else:
            if n == 'swc2': lines.append(f'b = {gte_read_expression(i.rt, False)};')
            lines += ['if (!timed) {']
            if partial:
                start = '0' if n == 'swl' else 'address & 3'
                end = '(address & 3) + 1' if n == 'swl' else '4'
                shift = '(3 - (address & 3) + j) * 8' if n == 'swl' else '(j - (address & 3)) * 8'
                lines += [f'unsigned j; for (j = {start}; j < {end}; ++j) {{',
                          f'uint32_t byte = b >> ({shift});',
                          'if (scratch) scratch_write(c, physical + j, 1, byte);',
                          'else ram_write(c, physical + j, 1, byte);', '}']
            else:
                lines += [f'if (scratch) scratch_write(c, physical, {width}, b);',
                          f'else ram_write(c, physical, {width}, b);']
            lines += ['} else {'] + self.observe(i, pending, count, branch, advance=False)
            if partial:
                lines += [f'c_count_recompiled(c, {u(self.f.entry)}, {count - 1} - ticked);',
                          f'c_recomp_budget(c, {count - 1} - ticked);',
                          'c->fault_instructions = 0;',
                          f'c_tick(c, {count - 1} - ticked);',
                          'c->fault_instructions = 1;', f'c_{n}(c, address, b);']
            else:
                lines += [f'mem_compiled_write(c, aligned, {width}, b, {count - 1} - ticked);']
            lines += [f'ticked = {count - 1};'] + self.observed(pending) + ['}']
            lines += self.retire(pending)
        # MMIO may move the next deadline. Retire the access before delivering
        # an IRQ; the continuation of a delay slot is its captured branch target.
        target = 'c->branch_target' if branch else u(i.pc + 4)
        lines += ['if (timed) {'] + self.tick(target, count, load, reset=False, inline=False) + ['}']
        if n in {'swl', 'swr'} and not branch:
            lines += [f'if (code_epoch != c->code_epoch && !code_valid(c, &code_{self.f.entry:08x})) {{']
            lines += self.tick(i.pc + 4, count, load, inline=False) + ['return;', '}', 'code_epoch = c->code_epoch;']
        return lines, load

    def jump(self, target, count, pending):
        if not self.poll_forward and target in self.body and target > self.block_start:
            # A delay-slot RAM store can change the forward successor's code.
            # Preserve the source barrier while carrying ordinary prefixes.
            lines = ['if (code_epoch != c->code_epoch) {']
            lines += self.tick(target, count, pending, reset=False, inline=False) + ['}']
            return lines + super().jump(target, count, pending)
        return super().jump(target, count, pending)

    def block(self, start, pending):
        lines = super().block(start, pending)
        if self.loose and pending == 0 and self.wait_loop(start):
            lines = ['c_timing_idle(c);'] + lines
        return lines

    def wait_loop(self, start):
        """A small load/ALU/backedge is a wait hint, never a purity proof."""
        seen_load = False
        for offset in range(0, 32, 4):
            i = self.body.get(start + offset)
            if i is None: return False
            if control(i):
                slot = self.body.get(i.pc + 4)
                return (seen_load and i.name in {'beq', 'bne', 'blez', 'bgtz', 'bltz', 'bgez'}
                        and i.target == start and slot is not None and slot.word == 0)
            if i.name in {'lb', 'lbu', 'lh', 'lhu', 'lw'}: seen_load = True
            elif i.name not in {'sll', 'srl', 'sra', 'lui', 'andi', 'ori', 'and', 'or', 'slt', 'sltu', 'slti', 'sltiu'}:
                return False
        return False
