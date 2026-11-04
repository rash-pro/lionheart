"""Differential countdown batching across every instruction boundary of an IRQ."""
import ctypes
from pathlib import Path
import struct
import subprocess

import pytest

from recomp.discover import Image, discover
from recomp.emit import emit
from test_recompiler import imm, r, j


@pytest.fixture(params=['compact', 'fast'])
def idle(tmp_path, request):
    base = 0x80010000
    # Countdown in RAM; break on timeout, otherwise compare an IRQ-owned clock.
    words = [imm(35, 20, 8, 0), 0, imm(9, 8, 8, -1), imm(43, 20, 8, 0),
        imm(35, 20, 8, 0), 0, imm(5, 8, 9, 3), 0, *[r(8, rs=31), 0],
        imm(15, 0, 8, 0x8010), imm(35, 8, 8, 4), 0, r(42, 8, 10, 8),
        imm(5, 8, 0, -15), 0, r(8, rs=31), 0]
    data = struct.pack(f'<{len(words)}I', *words)
    image = Image(data, base, base)
    emit(image, discover(image), tmp_path)
    (tmp_path / 'probe.c').write_text('''
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
void hw_event(cpu_t *c) { mem_write32(c, 0x80100004, 1); c->next_event = UINT64_MAX/2; }
uint64_t check(const void *data, unsigned interpret, unsigned deadline, unsigned counter, unsigned alias, uint32_t *out) {
    cpu_t *c = malloc(sizeof(*c)); uint64_t accelerated;
    cpu_init(c); memcpy(c->ram + 0x10000, data, 72); c_set_sr(c, 0x401);
    c->entries = generated_entries; c->entry_count = generated_count; c->entry_regions = generated_regions;
    c->r[20] = alias ? 0x80100004 : 0x80100000; c->r[9] = UINT32_MAX; c->r[10] = 1;
    mem_write32(c, 0x80100000, counter); mem_write32(c, 0x80100004, 0);
    c->interpreter_only = interpret; c->next_event = deadline; c->instruction_limit = c->stall_limit = 0;
    dispatch(c, 0x80010000);
    memcpy(out, c->r, 128); out[32] = c->pc; out[33] = c->load_reg; out[34] = c->load_value;
    out[35] = c->cycles; out[36] = mem_read32(c, 0x80100000); out[37] = c_instructions(c);
    out[38] = mem_read32(c, 0x80100004); out[39] = c->halted;
    accelerated = c->idle_instructions; free(c); return accelerated;
}
''')
    binary = tmp_path / 'probe.so'
    subprocess.run(['cc', '-std=c99', '-O2', '-DFF8_STATS', '-shared', '-fPIC',
        *(['-DFF8_COMPACT'] if request.param == 'compact' else []), '-Iruntime', '-I'+str(tmp_path),
        'runtime/cpu.c', 'runtime/interp.c', *map(str, tmp_path.glob('*.c')), '-o', str(binary)], check=True)
    lib = ctypes.CDLL(str(binary))
    lib.check.argtypes = [ctypes.c_void_p, ctypes.c_uint, ctypes.c_uint, ctypes.c_uint, ctypes.c_uint,
                          ctypes.POINTER(ctypes.c_uint32)]
    lib.check.restype = ctypes.c_uint64
    return lib, data


def test_idle_matches_every_irq_boundary(idle):
    lib, data = idle
    saw_batch = False
    for counter in (0, 1, 3, 10000):
        for deadline in range(1, 201):
            a, b = (ctypes.c_uint32 * 40)(), (ctypes.c_uint32 * 40)()
            batch = lib.check(data, 0, deadline, counter, 0, a)
            lib.check(data, 1, deadline, counter, 0, b)
            assert list(a) == list(b), (counter, deadline, list(a), list(b))
            saw_batch |= batch > 0
    assert saw_batch


def test_idle_rejects_counter_clock_alias(idle):
    lib, data = idle
    a, b = (ctypes.c_uint32 * 40)(), (ctypes.c_uint32 * 40)()
    assert lib.check(data, 0, 151, 1000, 1, a) == 0
    lib.check(data, 1, 151, 1000, 1, b)
    assert list(a) == list(b)


@pytest.mark.parametrize('direct', [False, True])
def test_idle_stays_compiled_across_masked_events(tmp_path, direct):
    # A real timer IRQ calls compiled guest code to update the loop's clock.
    # Masked SPU samples and HBlank timer clocks precede that IRQ many times.
    base = 0x80010000
    words = [imm(35, 20, 8, 0), 0, imm(9, 8, 8, -1), imm(43, 20, 8, 0),
        imm(35, 20, 8, 0), 0, imm(5, 8, 9, 3), 0, r(8, rs=31), 0,
        imm(15, 0, 8, 0x8010), imm(35, 8, 8, 4), 0, r(42, 8, 10, 8),
        imm(5, 8, 0, -15), 0, r(8, rs=31), 0]
    words += [0] * (64 - len(words))
    words += [imm(15, 0, 8, 0x8010), imm(9, 0, 9, 1), imm(43, 8, 9, 4), r(8, rs=31), 0]
    data = struct.pack(f'<{len(words)}I', *words)
    image = Image(data, base, base, entry_points=(base + 256,))
    emit(image, discover(image), tmp_path)
    (tmp_path / 'probe.c').write_text('''
#include "hw.h"
#include "generated.h"
#include <stdlib.h>
void check(const void *data, unsigned interpret, unsigned target, uint64_t *out) {
    cpu_t *c = malloc(sizeof(*c));
    cpu_init(c); memcpy(c->ram + 0x10000, data, DATA_SIZE);
    c->instruction_limit = c->stall_limit = 0; hw_get(c);
    c->entries = generated_entries; c->entry_count = generated_count; c->entry_regions = generated_regions;
    c->r[20] = 0x80100000; c->r[9] = UINT32_MAX; c->r[10] = 1;
    mem_write32(c, 0x80100000, 100000);
    c->hw->events[0] = (bios_event_t){0xf2000002u, 2, 0x1000, 0x80010100, 2};
    hw_write(c, 0x1f801114, 2, 0x100); /* Timer 1 clocks HBlank, masked. */
    hw_write(c, 0x1f801128, 2, target);
    hw_write(c, 0x1f801124, 2, 0x58);
    hw_write(c, 0x1f801074, 2, 0x40); c_set_sr(c, 0x401);
    c->interpreter_only = interpret;
    dispatch(c, 0x80010000);
    for (unsigned i = 0; i < 32; ++i) out[i] = c->r[i];
    out[32] = c->pc; out[33] = c->cycles; out[34] = c->load_reg; out[35] = c->load_value;
    out[36] = mem_read32(c, 0x80100000); out[37] = mem_read32(c, 0x80100004);
    out[38] = c_instructions(c); out[39] = c->halted;
    out[40] = c->interp_instructions; out[41] = c->idle_instructions;
    out[42] = c->hw->irq_delivered[6]; out[43] = c->hw->event_calls[HW_VIDEO];
    hw_destroy(c); free(c);
}
'''.replace('DATA_SIZE', str(len(data))))
    binary = tmp_path / 'probe.so'
    subprocess.run(['cc', '-std=c99', '-O2', '-DFF8_STATS', '-shared', '-fPIC',
        '-DFF8_COMPACT' if direct else '-DFF8_COMPACT=1', '-Iruntime', '-I'+str(tmp_path),
        *map(str, sorted(Path('runtime').glob('*.c'))), *map(str, tmp_path.glob('*.c')),
        '-o', str(binary)], check=True, capture_output=True)
    lib = ctypes.CDLL(str(binary))
    lib.check.argtypes = [ctypes.c_void_p, ctypes.c_uint, ctypes.c_uint, ctypes.POINTER(ctypes.c_uint64)]
    for target in range(9990, 10020):
        a, b = (ctypes.c_uint64 * 44)(), (ctypes.c_uint64 * 44)()
        lib.check(data, 0, target, a); lib.check(data, 1, target, b)
        assert list(a)[:40] == list(b)[:40], target
        assert a[37] == a[42] == 1 and a[43] >= 8
        assert a[40] < 32 and a[41] > 4000, (target, list(a))
