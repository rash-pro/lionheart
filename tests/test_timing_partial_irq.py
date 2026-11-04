"""A due IRQ retires the branch before its timed partial-store delay slot."""
import ctypes
import struct
import subprocess

import pytest

from recomp.discover import Image, discover
from recomp.emit import emit
from test_recompiler import imm, r


@pytest.mark.parametrize('mode', ['block', 'loose'])
@pytest.mark.parametrize('opcode', [42, 46], ids=['swl', 'swr'])
@pytest.mark.parametrize('cache', [False, True])
@pytest.mark.parametrize('stop', ['irq', 'missing-slot'])
def test_partial_store_irq_and_branch_stop_accounting(tmp_path, mode, opcode, cache, stop):
    words = [imm(4, 0, 0, 1), imm(opcode, 20, 8, 1), r(8, rs=31), 0]
    if stop == 'missing-slot':
        words = [imm(9, 0, 9, 1), 0, 0, imm(4, 0, 0, -1)]
    data = struct.pack('<4I', *words)
    image = Image(data, 0x80010000, 0x80010000)
    emit(image, discover(image), tmp_path, timing=mode,
         register_cache=dict(max_registers=8, functions=['f_80010000']) if cache else None)
    (tmp_path / 'probe.c').write_text('''
#include "cpu.h"
#include "generated.h"
#include <stdlib.h>
static unsigned writes;
void bios_call(cpu_t *c, unsigned t) { (void)c; (void)t; }
void bios_syscall(cpu_t *c) { (void)c; }
void gte_cmd(cpu_t *c, uint32_t v) { (void)c; (void)v; }
uint32_t gte_read(cpu_t *c, unsigned r, int t) { (void)t; return c->gte_data[r]; }
void gte_write(cpu_t *c, unsigned r, uint32_t v, int t) { (void)t; c->gte_data[r] = v; }
uint32_t hw_read(cpu_t *c, uint32_t a, unsigned w) { (void)c; (void)a; (void)w; return 0; }
void hw_write(cpu_t *c, uint32_t a, unsigned w, uint32_t v) {
    (void)c; (void)a; (void)w; (void)v; ++writes;
}
void hw_event(cpu_t *c) {
    c->cop0[13] |= 0x400;
    if (!c->in_delay_slot) cpu_stop(c, "IRQ before slot");
}
void check(const void *data, uint32_t *out) {
    cpu_t *c = malloc(sizeof(*c)); cpu_init(c); memcpy(c->ram + 0x10000, data, 16);
    c->entries = generated_entries; c->entry_count = generated_count; c->entry_regions = generated_regions;
    c->r[20] = 0x1f801814; c->r[8] = 0x12345678;
    c->instruction_limit = c->stall_limit = 0; writes = 0;
    c_set_sr(c, 0x401); c->next_event = 2; c->stop_armed = 1;
    if (!setjmp(c->stop)) dispatch(c, 0x80010000);
    out[0] = writes; out[1] = (uint32_t)c->cycles; out[2] = c->halted;
    free(c);
}
''')
    if stop == 'missing-slot':
        probe = tmp_path / 'probe.c'
        probe.write_text(probe.read_text().replace('c->next_event = 2;', 'c->next_event = 1000;'))
    binary = tmp_path / 'probe.so'
    subprocess.run(['cc', '-std=c99', '-O2', '-DFF8_COMPACT',
                    '-DFF8_TIMING_INLINE', *(['-DFF8_PLAY_REG_CACHE'] if cache else []),
                    '-shared', '-fPIC', '-Iruntime', '-I' + str(tmp_path),
                    'runtime/cpu.c', 'runtime/interp.c', 'runtime/timing.c',
                    *map(str, tmp_path.glob('*.c')), '-o', str(binary)], check=True, capture_output=True)
    lib = ctypes.CDLL(str(binary))
    lib.check.argtypes = [ctypes.c_void_p, ctypes.POINTER(ctypes.c_uint32)]
    out = (ctypes.c_uint32 * 3)()
    lib.check(data, out)
    assert list(out) == [0, 2 if stop == 'irq' else 8, 1]
