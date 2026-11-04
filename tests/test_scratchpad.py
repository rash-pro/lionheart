"""Scratchpad widths, aliases, load delays and event boundaries against the interpreter."""
import ctypes
from pathlib import Path
import struct
import subprocess

import pytest

from recomp.discover import Image, discover
from recomp.emit import emit
from test_recompiler import imm, r


@pytest.fixture(params=['compact', 'helper', 'fast', 'precise'])
def scratch(tmp_path, request):
    words = [imm(43, 20, 9, 0), imm(33, 20, 10, 2), imm(32, 20, 11, 1),
             imm(37, 20, 12, 0), imm(36, 20, 13, 3), imm(35, 20, 14, 0),
             imm(34, 20, 15, 3), imm(38, 20, 15, 0), imm(50, 20, 6, 0),
             imm(58, 20, 6, 4), r(8, rs=31), 0]
    data = struct.pack('<12I', *words)
    image = Image(data, 0x80010000, 0x80010000)
    emit(image, discover(image), tmp_path, host_mode='precise' if request.param == 'precise' else 'fast')
    (tmp_path/'probe.c').write_text('''
#include "cpu.h"
#include "generated.h"
#include <stdlib.h>
void bios_call(cpu_t *c, unsigned t) { (void)c; (void)t; }
void bios_syscall(cpu_t *c) { (void)c; }
void gte_cmd(cpu_t *c, uint32_t v) { (void)c; (void)v; }
uint32_t gte_read(cpu_t *c, unsigned r, int t) { (void)t; return c->gte_data[r]; }
void gte_write(cpu_t *c, unsigned r, uint32_t v, int t) { (void)t; c->gte_data[r] = v; }
uint32_t hw_read(cpu_t *c, uint32_t a, unsigned w) { (void)c; (void)a; (void)w; return 0; }
void hw_write(cpu_t *c, uint32_t a, unsigned w, uint32_t v) { (void)c; (void)a; (void)w; (void)v; }
void hw_event(cpu_t *c) {
    if ((c->cop0[12] & 0x401) == 0x401) mem_write32(c, 0x1f8003f8, 0x89abcdef);
    c->next_event = UINT64_MAX/2;
}
void check(const void *data, unsigned interpret, unsigned deadline, unsigned alias, unsigned sr, uint32_t *out) {
    cpu_t *c = malloc(sizeof(*c)); cpu_init(c); memcpy(c->ram + 0x10000, data, 48);
    c->entries = generated_entries; c->entry_count = generated_count; c->entry_regions = generated_regions;
    c->r[20] = alias; c->r[9] = 0x1234fedc; c_set_sr(c, sr);
    c->load_reg = 8; c->load_value = 0x87654321;
    c->interpreter_only = interpret; c->next_event = deadline; c->instruction_limit = c->stall_limit = 0;
    dispatch(c, 0x80010000);
    memcpy(out, c->r, 128); memcpy(out+32, c->cop0, 128); memcpy(out+64, c->gte_data, 128);
    out[96] = c->pc; out[97] = c->cycles; out[98] = c->load_reg; out[99] = c->load_value;
    out[100] = c_instructions(c); out[101] = c->halted; out[102] = c->branch_pending;
    memcpy(out+103, c->scratch, 1024); free(c);
}
''')
    binary = tmp_path/'probe.so'
    flags = {'compact': ['-DFF8_COMPACT'], 'helper': ['-DFF8_COMPACT=1']}.get(request.param, [])
    subprocess.run(['cc', '-std=c99', '-O2', '-DFF8_STATS', *flags, '-shared', '-fPIC',
                    '-Iruntime', '-I'+str(tmp_path), 'runtime/cpu.c', 'runtime/interp.c',
                    *map(str, tmp_path.glob('*.c')), '-o', str(binary)], check=True, capture_output=True)
    lib = ctypes.CDLL(str(binary))
    lib.check.argtypes = [ctypes.c_void_p, *[ctypes.c_uint]*4, ctypes.POINTER(ctypes.c_uint32)]
    return lib, data


def test_scratchpad_matches_every_event_boundary(scratch):
    lib, data = scratch
    for alias in (0x1f8003f8, 0x9f8003f8, 0xbf8003f8, 0x1f8003f9, 0x1f8003fc, 0x1f800400):
        for sr in (0, 0x401, 0x10401):
            for deadline in range(1, 30):
                a, b = (ctypes.c_uint32*359)(), (ctypes.c_uint32*359)()
                lib.check(data, 0, deadline, alias, sr, a)
                lib.check(data, 1, deadline, alias, sr, b)
                assert list(a) == list(b), (hex(alias), hex(sr), deadline)
