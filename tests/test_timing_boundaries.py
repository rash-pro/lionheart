"""Show the intentional deadline relaxation and preserved MMIO timestamps."""
import ctypes
import struct
import subprocess

import pytest

from recomp.discover import Image, discover
from recomp.emit import emit
from test_recompiler import imm, r


@pytest.fixture(params=['exact', 'block', 'loose', 'block-inline', 'loose-inline', 'block-cache', 'loose-cache'])
def timing(request, tmp_path):
    words = [imm(9, 0, 9, n) for n in range(1, 5)] + [imm(35, 20, 8, 0), 0, r(8, rs=31), 0]
    data = struct.pack('<8I', *words)
    image = Image(data, 0x80010000, 0x80010000)
    mode=request.param.split('-')[0]
    cache=dict(max_registers=8,functions=['f_80010000']) if request.param.endswith('-cache') else None
    emit(image, discover(image), tmp_path, timing=mode, register_cache=cache)
    if '-' in request.param:
        header=tmp_path/'generated.h'
        header.write_text('#define FF8_TIMING_INLINE 1\n' + ('#define FF8_PLAY_REG_CACHE 1\n' if cache else '') + header.read_text())
    (tmp_path / 'probe.c').write_text('''
#include "cpu.h"
#include "generated.h"
#include <stdlib.h>
static uint64_t event_cycle;
static uint64_t fence_cycle;
void bios_call(cpu_t *c, unsigned t) { (void)c; (void)t; }
void bios_syscall(cpu_t *c) { (void)c; }
void gte_cmd(cpu_t *c, uint32_t v) { (void)c; (void)v; }
uint32_t gte_read(cpu_t *c, unsigned r, int t) { (void)t; return c->gte_data[r]; }
void gte_write(cpu_t *c, unsigned r, uint32_t v, int t) { (void)t; c->gte_data[r] = v; }
uint32_t hw_read(cpu_t *c, uint32_t a, unsigned w) { (void)a; (void)w; return (uint32_t)c->cycles; }
void hw_write(cpu_t *c, uint32_t a, unsigned w, uint32_t v) { (void)c; (void)a; (void)w; (void)v; }
void hw_event(cpu_t *c) {
    event_cycle = c->cycles; c->r[9] = 77; ram_write(c, 0x100000, 4, 99); c->next_event = UINT64_MAX/2;
}
void hw_ram_fence(cpu_t *c, uint32_t p, uint32_t n) { (void)p; (void)n; fence_cycle = c->cycles; }
void check(const void *data, unsigned address, unsigned deadline, unsigned fenced, uint32_t *out) {
    cpu_t *c = malloc(sizeof(*c)); cpu_init(c); memcpy(c->ram + 0x10000, data, 32);
    c->entries = generated_entries; c->entry_count = generated_count; c->entry_regions = generated_regions;
    c->r[20] = address; c->instruction_limit = c->stall_limit = 0;
    if (fenced) c->ram_top = 0x100000;
    c_set_sr(c, 0x401); c->next_event = deadline; event_cycle = fence_cycle = 0;
    dispatch(c, 0x80010000);
    out[0] = c->r[8]; out[1] = (uint32_t)event_cycle; out[2] = (uint32_t)c->cycles;
    out[3] = (uint32_t)fence_cycle; out[4] = c->r[9]; free(c);
}
''')
    binary = tmp_path / 'probe.so'
    subprocess.run(['cc', '-std=c99', '-O2', '-DFF8_COMPACT', '-shared', '-fPIC',
                    '-Iruntime', '-I' + str(tmp_path), 'runtime/cpu.c', 'runtime/interp.c', 'runtime/timing.c',
                    *map(str, tmp_path.glob('*.c')), '-o', str(binary)], check=True, capture_output=True)
    lib = ctypes.CDLL(str(binary))
    lib.check.argtypes = [ctypes.c_void_p, ctypes.c_uint, ctypes.c_uint, ctypes.c_uint, ctypes.POINTER(ctypes.c_uint32)]
    return mode, lib, data


def test_ram_read_crosses_deadline_only_in_experiment(timing):
    mode, lib, data = timing
    out = (ctypes.c_uint32 * 5)()
    lib.check(data, 0x80100000, 4, 0, out)
    assert out[0] == (99 if mode == 'exact' else 0)
    assert out[1] == {'exact': 4, 'block': 16, 'loose': 0}[mode]
    assert out[2] == 16


def test_mmio_uses_static_instruction_offset(timing):
    _, lib, data = timing
    out = (ctypes.c_uint32 * 5)()
    lib.check(data, 0x1f801814, 1000, 0, out)
    assert out[0] == 8
    assert out[2] == 16


def test_media_ram_fence_flushes_static_prefix(timing):
    _, lib, data = timing
    out = (ctypes.c_uint32 * 5)()
    lib.check(data, 0x80100000, 1000, 1, out)
    assert out[3] == 8
    assert out[2] == 16


def test_event_updates_survive_cached_register_continuation(timing):
    mode, lib, data = timing
    out = (ctypes.c_uint32 * 5)()
    lib.check(data, 0x80100000, 4, 0, out)
    assert out[4] == (77 if mode == 'block' else 4)
