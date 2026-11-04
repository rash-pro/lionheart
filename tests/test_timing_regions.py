"""Selected forward regions retain prefix timestamps and materialize at events."""
import ctypes
import json
import struct
import subprocess

import pytest

from recomp.discover import Image, discover
from recomp.emit import emit
from recomp.locals import read_selection
from test_recompiler import imm, r


@pytest.fixture(params=[(False, False), (False, True), (True, False), (True, True)],
                ids=['block', 'block-cache', 'region', 'region-cache'])
def forward(request, tmp_path):
    fused, cache = request.param
    words = [imm(9, 0, 9, 1), imm(4, 0, 0, 2), imm(9, 0, 9, 2),
             imm(9, 0, 9, 99), imm(9, 0, 9, 3), imm(35, 20, 8, 0), 0, r(8, rs=31), 0]
    data = struct.pack('<9I', *words)
    image = Image(data, 0x80010000, 0x80010000)
    emit(image, discover(image), tmp_path, timing='block',
         register_cache=dict(max_registers=8, functions=['f_80010000'],
                             fuse_forward=['f_80010000'] if fused else []))
    (tmp_path / 'probe.c').write_text('''
#include "cpu.h"
#include "generated.h"
#include <stdlib.h>
static uint64_t event_cycle;
void bios_call(cpu_t *c, unsigned t) { (void)c; (void)t; }
void bios_syscall(cpu_t *c) { (void)c; }
void gte_cmd(cpu_t *c, uint32_t v) { (void)c; (void)v; }
uint32_t gte_read(cpu_t *c, unsigned r, int t) { (void)t; return c->gte_data[r]; }
void gte_write(cpu_t *c, unsigned r, uint32_t v, int t) { (void)t; c->gte_data[r] = v; }
uint32_t hw_read(cpu_t *c, uint32_t a, unsigned w) { (void)a; (void)w; return (uint32_t)c->cycles; }
void hw_write(cpu_t *c, uint32_t a, unsigned w, uint32_t v) { (void)c; (void)a; (void)w; (void)v; }
void hw_event(cpu_t *c) { event_cycle = c->cycles; c->r[9] = 77; c->next_event = UINT64_MAX / 2; }
void check(const void *data, unsigned deadline, uint32_t *out) {
    cpu_t *c = malloc(sizeof(*c)); cpu_init(c); memcpy(c->ram + 0x10000, data, 36);
    c->entries = generated_entries; c->entry_count = generated_count; c->entry_regions = generated_regions;
    c->r[20] = 0x1f801814; c->instruction_limit = c->stall_limit = 0;
    c_set_sr(c, 0x401); c->next_event = deadline; event_cycle = 0;
    dispatch(c, 0x80010000);
    out[0] = c->r[8]; out[1] = (uint32_t)event_cycle; out[2] = (uint32_t)c->cycles; out[3] = c->r[9];
    free(c);
}
''')
    binary = tmp_path / 'probe.so'
    subprocess.run(['cc', '-std=c99', '-O2', '-DFF8_COMPACT', '-DFF8_TIMING_INLINE',
                    *(['-DFF8_PLAY_REG_CACHE'] if cache else []), '-shared', '-fPIC',
                    '-Iruntime', '-I' + str(tmp_path), 'runtime/cpu.c', 'runtime/interp.c', 'runtime/timing.c',
                    *map(str, tmp_path.glob('*.c')), '-o', str(binary)], check=True, capture_output=True)
    lib = ctypes.CDLL(str(binary))
    lib.check.argtypes = [ctypes.c_void_p, ctypes.c_uint, ctypes.POINTER(ctypes.c_uint32)]
    return fused, lib, data


def test_mmio_flushes_the_complete_forward_prefix(forward):
    _, lib, data = forward
    out = (ctypes.c_uint32 * 4)()
    lib.check(data, 1000, out)
    assert list(out) == [8, 0, 16, 3]


def test_region_event_changes_survive_cached_continuation(forward):
    fused, lib, data = forward
    out = (ctypes.c_uint32 * 4)()
    lib.check(data, 4, out)
    assert list(out) == ([8, 8, 16, 77] if fused else [8, 6, 16, 3])


@pytest.mark.parametrize('regions', [None, 'f_80010000', {}, [[]], [{}],
                                     ['f_80010000', 'f_80010000'], ['f_80010004']])
def test_region_policy_rejects_invalid_or_unselected_owners(tmp_path, regions):
    path = tmp_path / 'policy.json'
    path.write_text(json.dumps(dict(max_registers=8, functions=['f_80010000'], fuse_forward=regions)))
    with pytest.raises(ValueError, match='fuse_forward'):
        read_selection(path)


def test_region_policy_retains_the_existing_selection(tmp_path):
    path = tmp_path / 'policy.json'
    expected = dict(max_registers=8, functions=['f_80010000'], fuse_forward=['f_80010000'])
    path.write_text(json.dumps(expected))
    assert read_selection(path) == expected


def test_region_manifest_reports_only_emitted_owners(tmp_path):
    data = struct.pack('<2I', r(8, rs=31), 0)
    image = Image(data, 0x80010000, 0x80010000)
    manifest = emit(image, discover(image), tmp_path, timing='block',
                    register_cache=dict(max_registers=8, functions=['f_80010000', 'f_80010010'],
                                        fuse_forward=['f_80010000', 'f_80010010']))
    assert manifest['forward_regions'] == ['f_80010000']


def test_region_manifest_covers_every_translation_unit(tmp_path):
    words = [imm(4, 0, 0, 2), 0, imm(9, 0, 9, 99), imm(9, 0, 9, 3), 0, r(8, rs=31), 0, 0]
    data = struct.pack('<8I', *words) * 50
    entries = tuple(0x80010000 + n * 32 for n in range(50))
    image = Image(data, entries[0], entries[0], entry_points=entries)
    selected = [f'f_{entries[0]:08x}', f'f_{entries[-1]:08x}']
    manifest = emit(image, discover(image), tmp_path, timing='block',
                    register_cache=dict(max_registers=8, functions=selected, fuse_forward=selected))
    assert manifest['forward_regions'] == selected


@pytest.mark.parametrize('cache', [False, True])
@pytest.mark.parametrize('opcode,offset,register', [(43, 0, 8), (42, 3, 8), (46, 0, 8), (58, 0, 23)],
                         ids=['sw', 'swl', 'swr', 'swc2'])
def test_forward_delay_store_invalidates_before_its_successor(tmp_path, cache, opcode, offset, register):
    words = [imm(4, 0, 0, 2), imm(opcode, 20, register, offset), imm(9, 0, 9, 99),
             imm(9, 0, 9, 1), r(8, rs=31), 0]
    data = struct.pack('<6I', *words)
    image = Image(data, 0x80010000, 0x80010000)
    emit(image, discover(image), tmp_path, timing='block',
         register_cache=dict(max_registers=8, functions=['f_80010000'], fuse_forward=['f_80010000']))
    (tmp_path / 'probe.c').write_text('''
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
void check(const void *data, uint32_t replacement, uint32_t *out) {
    cpu_t *c = malloc(sizeof(*c)); cpu_init(c); memcpy(c->ram + 0x10000, data, 24);
    c->entries = generated_entries; c->entry_count = generated_count; c->entry_regions = generated_regions;
    c->r[20] = 0x8001000c; c->r[8] = c->gte_data[23] = replacement;
    c->instruction_limit = c->stall_limit = 0; c->next_event = UINT64_MAX / 2;
    dispatch(c, 0x80010000);
    out[0] = c->r[9]; out[1] = (uint32_t)c->cycles; free(c);
}
''')
    binary = tmp_path / 'probe.so'
    subprocess.run(['cc', '-std=c99', '-O2', '-DFF8_COMPACT', '-DFF8_TIMING_INLINE',
                    *(['-DFF8_PLAY_REG_CACHE'] if cache else []), '-shared', '-fPIC',
                    '-Iruntime', '-I' + str(tmp_path), 'runtime/cpu.c', 'runtime/interp.c', 'runtime/timing.c',
                    *map(str, tmp_path.glob('*.c')), '-o', str(binary)], check=True, capture_output=True)
    lib = ctypes.CDLL(str(binary))
    lib.check.argtypes = [ctypes.c_void_p, ctypes.c_uint32, ctypes.POINTER(ctypes.c_uint32)]
    out = (ctypes.c_uint32 * 2)()
    lib.check(data, imm(9, 0, 9, 77), out)
    assert list(out) == [77, 10]


@pytest.mark.parametrize('size', [512, 513])
def test_forward_regions_have_a_bounded_guest_prefix(tmp_path, size):
    data = struct.pack('<' + 'I' * size, *([imm(9, 8, 8, 1)] * (size - 2) + [r(8, rs=31), 0]))
    image = Image(data, 0x80010000, 0x80010000)
    selection = dict(max_registers=8, functions=['f_80010000'], fuse_forward=['f_80010000'])
    if size == 513:
        with pytest.raises(ValueError, match='exceeds 512 instructions'):
            emit(image, discover(image), tmp_path, timing='block', register_cache=selection)
    else:
        assert emit(image, discover(image), tmp_path, timing='block',
                    register_cache=selection)['forward_regions'] == ['f_80010000']
