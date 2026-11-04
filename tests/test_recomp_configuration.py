"""Small executable checks for the stats-off runtime and aligned guest loads."""
import ctypes as C
import struct
import subprocess

import pytest

from recomp.discover import Image, discover
from recomp.emit import emit


@pytest.mark.parametrize('direct', [False, True])
def test_compact_deadlines_preserve_loads_branches_and_resume(tmp_path, direct):
    # A load feeds a branch, its slot loads again, and the target calls a helper.
    # MMIO can shorten the deadline after compiled execution has already begun.
    words = [0x03E0B021, 0x8E880000, 0x01004821, 0x8EAA0000,
             0x11400002, 0x8E8B0004, 0x240C0063, 0x01606821,
             0x0C004010, 0x01607021, 0x02C0F821, 0x03E00008, 0,
             0, 0, 0, 0x25EF0001, 0x03E00008, 0]
    data = struct.pack(f'<{len(words)}I', *words)
    image = Image(data, 0x80010000, 0x80010000)
    emit(image, discover(image), tmp_path)
    (tmp_path / 'boundary.c').write_text('''
#include "cpu.h"
#include "generated.h"
#include <stdlib.h>
void bios_call(cpu_t *c, unsigned t) { (void)c; (void)t; }
void bios_syscall(cpu_t *c) { (void)c; }
void gte_cmd(cpu_t *c, uint32_t v) { (void)c; (void)v; }
uint32_t gte_read(cpu_t *c, unsigned r, int t) { (void)c; (void)r; (void)t; return 0; }
void gte_write(cpu_t *c, unsigned r, uint32_t v, int t) { (void)c; (void)r; (void)v; (void)t; }
uint32_t hw_read(cpu_t *c, uint32_t a, unsigned w) {
    (void)a; (void)w;
    if (c->seed) c->next_event = c->cycles + c->seed;
    return 123;
}
void hw_write(cpu_t *c, uint32_t a, unsigned w, uint32_t v) { (void)c; (void)a; (void)w; (void)v; }
void hw_event(cpu_t *c) {
    uint32_t *record = (uint32_t *)c->scratch;
    unsigned offset = 1 + record[0] * 38;
    if (++record[0] > 2) abort();
    memcpy(record + offset, c->r, sizeof(c->r));
    record[offset + 32] = c->pc; record[offset + 33] = c->cycles;
    record[offset + 34] = c->load_reg; record[offset + 35] = c->load_value;
    record[offset + 36] = c->branch_pending;
    record[offset + 37] = c->branch_pending ? c->branch_target : 0;
    c->next_event = UINT64_MAX / 2;
}
int check(const void *data, unsigned interpret, unsigned deadline, unsigned scheduled, uint32_t *out) {
    cpu_t *c = malloc(sizeof(*c));
    cpu_init(c); memcpy(c->ram + 0x10000, data, DATA_SIZE);
    c->entries = generated_entries; c->entry_count = generated_count; c->entry_regions = generated_regions;
    c->r[20] = 0x80100000; c->r[21] = 0x1f801000;
    mem_write32(c, 0x80100000, 77); mem_write32(c, 0x80100004, 88);
    c_set_sr(c, 0x401); c->seed = scheduled;
    c->interpreter_only = interpret; c->next_event = deadline;
    c->instruction_limit = c->stall_limit = 0;
    dispatch(c, 0x80010000);
    memcpy(out, c->scratch, 77 * sizeof(*out));
    memcpy(out + 77, c->r, sizeof(c->r));
    out[109] = c->pc; out[110] = c->cycles;
    out[111] = c->load_reg; out[112] = c->load_value; out[113] = c->branch_pending;
    out[114] = c_instructions(c);
    out[115] = c->interp_instructions; out[116] = c->recomp_instructions;
    int result = !c->halted && c->pc == 0xffffffffu;
    free(c); return result;
}
'''.replace('DATA_SIZE', str(len(data))))
    output = tmp_path / 'boundary.so'
    subprocess.run(['cc', '-std=c99', '-O2', '-DFF8_STATS', '-shared', '-fPIC',
                    '-DFF8_COMPACT' if direct else '-DFF8_COMPACT=1',
                    '-Iruntime', '-I' + str(tmp_path), 'runtime/cpu.c', 'runtime/interp.c',
                    *map(str, tmp_path.glob('*.c')), '-o', str(output)], check=True, capture_output=True)
    library = C.CDLL(str(output))
    library.check.argtypes = [C.c_void_p, C.c_uint, C.c_uint, C.c_uint, C.POINTER(C.c_uint32)]
    for deadline in range(1, 40):
        for scheduled in (0, 2, 4, 6, 10, 30):
            compiled, reference = (C.c_uint32 * 117)(), (C.c_uint32 * 117)()
            assert library.check(data, 1, deadline, scheduled, reference) == 1
            assert library.check(data, 0, deadline, scheduled, compiled) == 1
            assert list(compiled)[:115] == list(reference)[:115], (deadline, scheduled)
            assert compiled[115] + compiled[116] == reference[115] == 15
            if deadline == 39 and scheduled == 0:
                assert compiled[115] == 0 and compiled[116] == 15
            if deadline == 2:
                assert 0 < compiled[115] < 15 and compiled[116] > 0


@pytest.mark.parametrize('mode', ['fast', 'compact', 'trace', 'precise'])
@pytest.mark.parametrize('kind', ['delay_store', 'alu', 'bios', 'enable', 'rfe'])
def test_irq_observes_state_before_next_instruction(tmp_path, mode, kind):
    words = {
        'delay_store': [0x24080063, 0x10000001, 0xAE880000, 0x03E00008, 0],
        'alu': [0x24080063, 0x2409004D, 0x03E00008, 0],
        'bios': [0x03E0F021, 0x0C00002C, 0, 0x2409004D, 0x03C00008, 0],
        'rfe': [0x24080063, 0x42000010, 0x2409004D, 0x03E00008, 0],
        'enable': [0x24080063, 0x408C6000, 0x2409004D, 0x03E00008, 0],
    }[kind]
    data = struct.pack(f'<{len(words)}I', *words)
    image = Image(data, 0x80010000, 0x80010000)
    emit(image, discover(image), tmp_path, mode='trace' if mode == 'trace' else 'fast',
         host_mode='precise' if mode == 'precise' else 'fast')
    (tmp_path / 'slot.c').write_text('''
#include "cpu.h"
#include "generated.h"
#include <stdlib.h>
void bios_call(cpu_t *c, unsigned t) { (void)t; c->r[10] = 123; }
void bios_syscall(cpu_t *c) { (void)c; }
void gte_cmd(cpu_t *c, uint32_t v) { (void)c; (void)v; }
uint32_t gte_read(cpu_t *c, unsigned r, int t) { (void)c; (void)r; (void)t; return 0; }
void gte_write(cpu_t *c, unsigned r, uint32_t v, int t) { (void)c; (void)r; (void)v; (void)t; }
uint32_t hw_read(cpu_t *c, uint32_t a, unsigned w) { (void)c; (void)a; (void)w; return 0; }
void hw_write(cpu_t *c, uint32_t a, unsigned w, uint32_t v) { (void)c; (void)a; (void)w; (void)v; }
void hw_event(cpu_t *c) {
    c->cop0[13] |= 0x400;
    if (c->in_delay_slot || (c->cop0[12] & 0x401) != 0x401) { c->next_event = c->cycles + 2; return; }
#if defined(FF8_TEST_IRQ_ALU) || defined(FF8_TEST_IRQ_BIOS) || defined(FF8_TEST_IRQ_ENABLE) || defined(FF8_TEST_IRQ_RFE)
    mem_write32(c, 0x80100004, c->r[9]);
#else
    mem_write32(c, 0x80100004, mem_read32(c, 0x80100000));
#endif
    c->cop0[13] &= ~0x400u; c->next_event = UINT64_MAX / 2;
}
int check(const void *data, unsigned interpret) {
    cpu_t *c = malloc(sizeof(*c)); int result;
    cpu_init(c); memcpy(c->ram + 0x10000, data, DATA_SIZE); c_set_sr(c, 0x401);
#ifdef FF8_TEST_IRQ_ENABLE
    c_set_sr(c, 0); c->r[12] = 0x401;
#endif
#ifdef FF8_TEST_IRQ_RFE
    c_set_sr(c, 0x404);
#endif
    c->entries = generated_entries; c->entry_count = generated_count; c->entry_regions = generated_regions;
    c->r[20] = 0x80100000; mem_write32(c, 0x80100000, 55);
    c->interpreter_only = interpret; c->next_event = IRQ_CYCLE; c->instruction_limit = c->stall_limit = 0;
    dispatch(c, 0x80010000);
#ifdef FF8_TEST_IRQ_ALU
    result = !c->halted && c->cycles == 8 && c->r[9] == 77 && mem_read32(c, 0x80100004) == 0;
#elif defined(FF8_TEST_IRQ_BIOS)
    result = !c->halted && c->cycles == 12 && c->r[9] == 77 && c->r[10] == 123 && mem_read32(c, 0x80100004) == 77;
#elif defined(FF8_TEST_IRQ_ENABLE) || defined(FF8_TEST_IRQ_RFE)
    result = !c->halted && c->cycles == 10 && c->r[9] == 77 && mem_read32(c, 0x80100004) == 0;
#else
    result = !c->halted && c->cycles == 10 && mem_read32(c, 0x80100000) == 99 && mem_read32(c, 0x80100004) == 55;
#endif
    free(c); return result;
}
'''.replace('DATA_SIZE', str(len(data))).replace('IRQ_CYCLE', str({'delay_store': 4, 'alu': 2, 'bios': 8, 'enable': 2, 'rfe': 2}[kind])))
    output = tmp_path / 'slot.so'
    subprocess.run(['cc', '-std=c99', '-O2', '-DFF8_STATS', '-shared', '-fPIC',
                    *(['-DFF8_COMPACT'] if mode == 'compact' else []), '-Iruntime', '-I' + str(tmp_path),
                    '-DFF8_TEST_IRQ_' + kind.upper(),
                    'runtime/cpu.c', 'runtime/interp.c', *map(str, tmp_path.glob('*.c')),
                    '-o', str(output)], check=True, capture_output=True)
    library = C.CDLL(str(output)); library.check.argtypes = [C.c_void_p, C.c_uint]
    assert library.check(data, 1) == 1
    assert library.check(data, 0) == 1


@pytest.mark.parametrize('mode', ['fast', 'compact', 'trace', 'precise'])
@pytest.mark.parametrize('access', ['read', 'write', 'gtewrite'])
def test_async_dma_is_visible_before_compiled_ram_access(tmp_path, mode, access):
    write = access != 'read'
    gte = access == 'gtewrite'
    words = ([0x24080063, 0x24090007, 0xAE880000, 0x03E00008, 0] if write else
             [0x24090007, 0x240A0008, 0x8E880000, 0, 0xAE880004, 0x03E00008, 0])
    if gte: words[2] = 0xEA880000
    data = struct.pack(f'<{len(words)}I', *words)
    image = Image(data, 0x80010000, 0x80010000)
    emit(image, discover(image), tmp_path, mode='trace' if mode == 'trace' else 'fast',
         host_mode='precise' if mode == 'precise' else 'fast')
    (tmp_path / 'dma.c').write_text('''
#include "cpu.h"
#include "generated.h"
#include <stdlib.h>
void bios_call(cpu_t *c, unsigned t) { (void)c; (void)t; }
void bios_syscall(cpu_t *c) { (void)c; }
void gte_cmd(cpu_t *c, uint32_t v) { (void)c; (void)v; }
uint32_t gte_read(cpu_t *c, unsigned r, int t) { (void)t; return c->gte_data[r]; }
void gte_write(cpu_t *c, unsigned r, uint32_t v, int t) { (void)c; (void)r; (void)v; (void)t; }
uint32_t hw_read(cpu_t *c, uint32_t a, unsigned w) { (void)c; (void)a; (void)w; return 0; }
void hw_write(cpu_t *c, uint32_t a, unsigned w, uint32_t v) { (void)c; (void)a; (void)w; (void)v; }
void hw_event(cpu_t *c) { mem_write32(c, 0x80100000, 123); c->gte_data[8] = 123; c->next_event = UINT64_MAX / 2; }
int check(const void *data, unsigned interpret) {
    cpu_t *c = malloc(sizeof(*c));
    int result;
    cpu_init(c); memcpy(c->ram + 0x10000, data, DATA_BYTES);
    c->entries = generated_entries; c->entry_count = generated_count; c->entry_regions = generated_regions;
    c->r[20] = 0x80100000; mem_write32(c, 0x80100000, 55);
    c->gte_data[8] = 99;
    c->interpreter_only = interpret; c->next_event = 2; c->instruction_limit = c->stall_limit = 0;
    dispatch(c, 0x80010000);
    result = !c->halted && c->pc == 0xffffffffu && c->cycles == EXPECTED_CYCLES &&
             mem_read32(c, CHECK_ADDRESS) == EXPECTED_VALUE;
    free(c); return result;
}
'''.replace('DATA_BYTES', str(len(data))).replace('EXPECTED_CYCLES', str(len(words) * 2))
   .replace('CHECK_ADDRESS', '0x80100000' if write else '0x80100004')
   .replace('EXPECTED_VALUE', '99' if write and not gte else '123'))
    output = tmp_path / 'dma.so'
    subprocess.run(['cc', '-std=c99', '-O2', '-DFF8_STATS', '-shared', '-fPIC',
                    *(['-DFF8_COMPACT'] if mode == 'compact' else []), '-Iruntime', '-I' + str(tmp_path),
                    'runtime/cpu.c', 'runtime/interp.c', *map(str, tmp_path.glob('*.c')),
                    '-o', str(output)], check=True, capture_output=True)
    library = C.CDLL(str(output))
    library.check.argtypes = [C.c_void_p, C.c_uint]
    assert library.check(data, 1) == 1
    assert library.check(data, 0) == 1


@pytest.mark.parametrize('stats', [False, True])
@pytest.mark.parametrize('host_mode', ['fast', 'precise'])
def test_instruction_limit_without_statistics(tmp_path, stats, host_mode):
    base = 0x80010000
    # addiu t0,t0,-1; bne t0,zero,loop; addiu t1,t1,1; jr ra; nop
    data = struct.pack('<5I', 0x2508FFFF, 0x1500FFFE, 0x25290001, 0x03E00008, 0)
    image = Image(data, base, base)
    emit(image, discover(image), tmp_path, host_mode=host_mode)
    source = tmp_path / 'probe.c'
    source.write_text('''
#include "cpu.h"
#include "generated.h"
#include <stdlib.h>
void bios_call(cpu_t *c, unsigned t) { (void)c; (void)t; }
void bios_syscall(cpu_t *c) { (void)c; }
void gte_cmd(cpu_t *c, uint32_t v) { (void)c; (void)v; }
uint32_t gte_read(cpu_t *c, unsigned r, int t) { (void)c; (void)r; (void)t; return 0; }
void gte_write(cpu_t *c, unsigned r, uint32_t v, int t) { (void)c; (void)r; (void)v; (void)t; }
uint32_t hw_read(cpu_t *c, uint32_t a, unsigned w) { (void)c; (void)a; (void)w; return 0; }
void hw_write(cpu_t *c, uint32_t a, unsigned w, uint32_t v) { (void)c; (void)a; (void)w; (void)v; }
int probe(const void *data) {
    cpu_t *c = malloc(sizeof(*c));
    int result;
    cpu_init(c);
    memcpy(c->ram + 0x10000, data, 20);
    c->entries = generated_entries; c->entry_count = generated_count;
    c->entry_regions = generated_regions;
    c->r[8] = 1000000; c->instruction_limit = 10000; c->stall_limit = 0;
    dispatch(c, 0x80010000);
    result = c->halted && !strcmp(c->stop_reason, "instruction budget exhausted") &&
             c->cycles >= 20000 && c->cycles < 20008 &&
             !((uintptr_t)c->ram & 15);
#ifdef FF8_STATS
    result &= c->recomp_instructions > 0 && c->recomp_functions > 0;
#else
    result &= !c->recomp_instructions && !c->recomp_functions;
#endif
    free(c); return result;
}
''')
    output = tmp_path / 'probe.so'
    command = ['cc', '-std=c99', '-O2', '-shared', '-fPIC', '-fsanitize=undefined',
               '-fno-sanitize-recover=all', '-Iruntime', '-I' + str(tmp_path)]
    if stats:
        command.append('-DFF8_STATS')
    command += ['runtime/cpu.c', 'runtime/interp.c', *map(str, tmp_path.glob('*.c')), '-o', str(output)]
    subprocess.run(command, check=True, capture_output=True)
    library = C.CDLL(str(output))
    library.probe.argtypes = [C.c_void_p]
    assert library.probe(data) == 1


def test_compiled_events_resume_and_mmio_uses_static_cycle_offset(tmp_path):
    # Loop, an ALU instruction, then a clock-valued MMIO load and RAM store.
    data = struct.pack('<10I', 0x2508FFFF, 0x1500FFFE, 0, 0x240A0007,
                       0x8E890000, 0, 0xAEA90000, 0x03E00008, 0, 0)
    image = Image(data, 0x80010000, 0x80010000)
    emit(image, discover(image), tmp_path)
    (tmp_path / 'clock.c').write_text('''
#include "cpu.h"
#include "generated.h"
#include <stdlib.h>
void bios_call(cpu_t *c, unsigned t) { (void)c; (void)t; }
void bios_syscall(cpu_t *c) { (void)c; }
void gte_cmd(cpu_t *c, uint32_t v) { (void)c; (void)v; }
uint32_t gte_read(cpu_t *c, unsigned r, int t) { (void)c; (void)r; (void)t; return 0; }
void gte_write(cpu_t *c, unsigned r, uint32_t v, int t) { (void)c; (void)r; (void)v; (void)t; }
uint32_t hw_read(cpu_t *c, uint32_t a, unsigned w) { (void)a; (void)w; return c->cycles; }
void hw_write(cpu_t *c, uint32_t a, unsigned w, uint32_t v) { (void)c; (void)a; (void)w; (void)v; }
void hw_event(cpu_t *c) { c->mmio_epoch = c->cycles; c->next_event = UINT64_MAX / 2; }
int check(const void *data, unsigned interpret) {
    cpu_t *c = malloc(sizeof(*c));
    int result;
    cpu_init(c); memcpy(c->ram + 0x10000, data, 40);
    c->entries = generated_entries; c->entry_count = generated_count;
    c->entry_regions = generated_regions;
    c->r[8] = 3; c->r[20] = 0x1f801000; c->r[21] = 0x80100000;
    c->instruction_limit = 10000; c->stall_limit = 0;
    c->interpreter_only = interpret; c->next_event = 10; c_set_sr(c, 0x401);
    dispatch(c, 0x80010000);
    result = !c->halted && c->mmio_epoch == 10 && c->cycles == 30 &&
             mem_read32(c, 0x80100000) == 20 &&
             c_instructions(c) == 15 && (interpret || c->recomp_instructions > 0);
    free(c); return result;
}
''')
    output = tmp_path / 'clock.so'
    subprocess.run(['cc', '-std=c99', '-O2', '-DFF8_STATS', '-shared', '-fPIC',
                    '-Iruntime', '-I' + str(tmp_path), 'runtime/cpu.c', 'runtime/interp.c',
                    *map(str, tmp_path.glob('*.c')), '-o', str(output)], check=True, capture_output=True)
    library = C.CDLL(str(output))
    library.check.argtypes = [C.c_void_p, C.c_uint]
    assert library.check(data, 1) == 1
    assert library.check(data, 0) == 1


@pytest.mark.parametrize('nonlocal_handler', [False, True])
@pytest.mark.parametrize('cop2_load', [False, True])
@pytest.mark.parametrize('compact', [False, True])
@pytest.mark.parametrize('observe_pending', [False, True])
def test_mmio_irq_dispatch_returns_to_compiled_caller(tmp_path, nonlocal_handler, cop2_load, compact, observe_pending):
    # MMIO schedules an IRQ at retirement of the load. The compiled handler
    # observes the intermediate t1, then the interrupted function continues.
    words = [0x24090001, 0x8E880000, 0x25290001, 0xAEA90000, 0x03E00008, 0]
    if cop2_load: words[1] = 0xCA880000
    handler = [0xAEA90004, 0x24090063, 0x03E00008, 0]
    if nonlocal_handler:
        # Skip a second call by returning to its compiled continuation. The
        # callback must unwind without discarding hw_event's saved registers.
        handler = [0x03E07821, 0xAEA90004, 0x0C004018, 0, 0x0C004018, 0,
                   0x24090063, 0x01E0F821, 0x03E00008, 0, 0, 0,
                   0x3C1F8001, 0x37FF0048, 0x03E00008, 0]
    if observe_pending: handler[1 if nonlocal_handler else 0] = 0xAEA80004
    blob = words + [0] * 6 + handler
    data = struct.pack(f'<{len(blob)}I', *blob)
    image = Image(data, 0x80010000, 0x80010000)
    emit(image, discover(image, [0x80010030]), tmp_path)
    (tmp_path / 'irq.c').write_text('''
#include "cpu.h"
#include "generated.h"
#include <stdlib.h>
void bios_call(cpu_t *c, unsigned t) { (void)c; (void)t; }
void bios_syscall(cpu_t *c) { (void)c; }
void gte_cmd(cpu_t *c, uint32_t v) { (void)c; (void)v; }
uint32_t gte_read(cpu_t *c, unsigned r, int t) { (void)c; (void)r; (void)t; return 0; }
void gte_write(cpu_t *c, unsigned r, uint32_t v, int t) { (void)t; c->gte_data[r] = v; }
uint32_t hw_read(cpu_t *c, uint32_t a, unsigned w) {
    (void)a; (void)w; c->next_event = c->cycles + 2; return 123;
}
void hw_write(cpu_t *c, uint32_t a, unsigned w, uint32_t v) { (void)c; (void)a; (void)w; (void)v; }
void hw_event(cpu_t *c) {
    uint32_t regs[32], pc = c->pc, load = c->load_reg, value = c->load_value;
    c->next_event = UINT64_MAX / 2;
    memcpy(regs, c->r, sizeof(regs));
    c->r[31] = 0xfffffff0u; c->load_reg = c->load_value = 0;
    c_enter(c, 0x80010030, 0xfffffff0u); dispatch(c, 0x80010030); c_leave(c);
    memcpy(c->r, regs, sizeof(regs)); c->pc = pc;
    c->load_reg = load; c->load_value = value;
}
int check(const void *data, unsigned interpret) {
    cpu_t *c = malloc(sizeof(*c));
    int result;
    cpu_init(c); memcpy(c->ram + 0x10000, data, 64);
    c->interpreter_only = interpret;
    c->entries = generated_entries; c->entry_count = generated_count;
    c->entry_regions = generated_regions;
    c->r[20] = 0x1f801000; c->r[21] = 0x80100000;
    c->next_event = UINT64_MAX / 2; c->instruction_limit = c->stall_limit = 0;
    dispatch(c, 0x80010000);
    result = !c->halted && c->pc == 0xffffffffu && c->r[9] == 2 && c->r[8] == 123 &&
             mem_read32(c, 0x80100000) == 2 && mem_read32(c, 0x80100004) == 1 &&
             (interpret ? c->interp_instructions == 10 :
              !c->interp_instructions && c->recomp_instructions == 10) && c->cycles == 20;
    free(c); return result;
}
'''.replace('data, 64', f'data, {len(data)}')
   .replace('c->r[8] == 123', 'c->gte_data[8] == 123' if cop2_load else 'c->r[8] == 123')
   .replace('mem_read32(c, 0x80100004) == 1', f'mem_read32(c, 0x80100004) == {0 if observe_pending else 1}')
   .replace('== 10', '== 18' if nonlocal_handler else '== 10')
   .replace('== 20', '== 36' if nonlocal_handler else '== 20'))
    output = tmp_path / 'irq.so'
    subprocess.run(['cc', '-std=c99', '-O2', '-DFF8_STATS', '-shared', '-fPIC',
                    *(['-DFF8_COMPACT'] if compact else []),
                    '-Iruntime', '-I' + str(tmp_path), 'runtime/cpu.c', 'runtime/interp.c',
                    *map(str, tmp_path.glob('*.c')), '-o', str(output)], check=True, capture_output=True)
    library = C.CDLL(str(output))
    library.check.argtypes = [C.c_void_p, C.c_uint]
    assert library.check(data, 1) == 1
    assert library.check(data, 0) == 1
