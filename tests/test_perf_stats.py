"""Diagnostic paths and exact per-PC counts remain separate from guest state."""
import ctypes
import csv
from pathlib import Path
import struct
import subprocess

import pytest

from recomp.discover import Image, discover
from recomp.emit import emit
from test_recompiler import imm, r


@pytest.mark.parametrize('stats', [False, True])
def test_memory_and_transfer_counters(tmp_path, stats):
    source = tmp_path / 'probe.c'
    source.write_text('''
#include "cpu.h"
#include <stdlib.h>
void bios_call(cpu_t *c, unsigned t) { (void)c; (void)t; }
void bios_syscall(cpu_t *c) { (void)c; }
void hw_event(cpu_t *c) { c->next_event = UINT64_MAX / 2; }
uint32_t hw_read(cpu_t *c, uint32_t a, unsigned w) { (void)c; (void)a; (void)w; return 0; }
void hw_write(cpu_t *c, uint32_t a, unsigned w, uint32_t v) { (void)c; (void)a; (void)w; (void)v; }
void gte_cmd(cpu_t *c, uint32_t v) { (void)c; (void)v; }
uint32_t gte_read(cpu_t *c, unsigned r, int t) { (void)c; (void)r; (void)t; return 0; }
void gte_write(cpu_t *c, unsigned r, uint32_t v, int t) { (void)c; (void)r; (void)v; (void)t; }
void check(uint64_t *out) {
    cpu_t *c = malloc(sizeof(*c));
    const code_span_t span = {0x80010000, 4};
    const code_region_t region = {&span, 1, 0, 0, NULL};
    memory_site_t site = {0x80010000, 0, 0, 0, 0, 8, 9, 35, 1, &region};
    cpu_init(c); c->next_event = UINT64_MAX/2;
    c->r[8] = 0x80100000; c_memory(c, &site, 0, 0);
    c->r[8] = 0x1f800000; c_memory(c, &site, 0, 0);
    c->r[8] = 0x1f801070; c_memory(c, &site, 0, 0);
    c->r[8] = 0x80100000; c->next_event = c->cycles + 2; c_memory(c, &site, 0, 0);
    c->ram_top = c->ram_limit = 0x100000; c_memory(c, &site, 0, 0);
    c->ram_top = c->ram_limit = 0x800000; c->code_watched[0x100000 >> CODE_PAGE_SHIFT] = 1;
    site.op = 43; c_memory(c, &site, 0, 0);
    c_transfer_stats(c, 0x80010000, 0x80020000, C_DISPATCH_CALL);
    c_transfer_stats(c, 0x80020000, 0x80010008, C_DISPATCH_RETURN);
    c_transfer_stats(c, 0x80010020, 0x80010000, C_DISPATCH_TABLE);
    for (unsigned i = 0; i < C_MEM_COUNT; ++i) out[i] = c->perf.memory[i];
    for (unsigned i = 0; i < C_DISPATCH_COUNT; ++i) out[C_MEM_COUNT+i] = c->perf.dispatch[i];
    free(c);
}
''')
    binary = tmp_path / 'probe.so'
    subprocess.run(['cc', '-std=c99', '-O2', '-shared', '-fPIC', '-Iruntime',
                    *(['-DFF8_STATS'] if stats else []), 'runtime/cpu.c', 'runtime/interp.c',
                    str(source), '-o', str(binary)], check=True, capture_output=True)
    lib = ctypes.CDLL(str(binary))
    counts = (ctypes.c_uint64 * 11)()
    lib.check(counts)
    assert list(counts) == ([3, 1, 1, 1, 1, 1, 1, 1, 1, 1, 2] if stats else [0] * 11)


def test_compact_pc_histogram_matches_interpreter(tmp_path, monkeypatch):
    base = 0x80010000
    words = [imm(9, 8, 8, -1), imm(5, 8, 0, -2), 0, r(8, rs=31), 0]
    data = struct.pack('<5I', *words)
    image = Image(data, base, base)
    emit(image, discover(image), tmp_path)
    (tmp_path / 'probe.c').write_text('''
#include "cpu.h"
#include "generated.h"
#include <stdlib.h>
void bios_call(cpu_t *c, unsigned t) { (void)c; (void)t; }
void bios_syscall(cpu_t *c) { (void)c; }
void hw_event(cpu_t *c) { c->next_event = UINT64_MAX/2; }
uint32_t hw_read(cpu_t *c, uint32_t a, unsigned w) { (void)c; (void)a; (void)w; return 0; }
void hw_write(cpu_t *c, uint32_t a, unsigned w, uint32_t v) { (void)c; (void)a; (void)w; (void)v; }
void gte_cmd(cpu_t *c, uint32_t v) { (void)c; (void)v; }
uint32_t gte_read(cpu_t *c, unsigned r, int t) { (void)c; (void)r; (void)t; return 0; }
void gte_write(cpu_t *c, unsigned r, uint32_t v, int t) { (void)c; (void)r; (void)v; (void)t; }
void check(const void *data, unsigned interpret) {
    cpu_t *c = malloc(sizeof(*c)); cpu_init(c); memcpy(c->ram + 0x10000, data, 20);
    c->entries = generated_entries; c->entry_count = generated_count; c->entry_regions = generated_regions;
    c->r[8] = 3; c->interpreter_only = interpret; c->next_event = UINT64_MAX/2;
    c_perf_checkpoint(c, 10); dispatch(c, 0x80010000); c_perf_checkpoint(c, 11);
    free(c);
}
''')
    binary = tmp_path / 'probe.so'
    subprocess.run(['cc', '-std=c99', '-O2', '-DFF8_STATS', '-DFF8_COMPACT', '-shared', '-fPIC',
                    '-Iruntime', '-I'+str(tmp_path), 'runtime/cpu.c', 'runtime/interp.c',
                    *map(str, tmp_path.glob('*.c')), '-o', str(binary)], check=True, capture_output=True)
    lib = ctypes.CDLL(str(binary))
    monkeypatch.setenv('FF8_PC_FIRST', '10'); monkeypatch.setenv('FF8_PC_LAST', '11')
    histograms = []
    for interpret in (0, 1):
        path = tmp_path / f'{interpret}.csv'
        monkeypatch.setenv('FF8_PC_PROFILE', str(path))
        lib.check(data, interpret)
        histograms.append(list(csv.DictReader(path.open())))
    assert histograms[0] == histograms[1] == [
        {'pc': f'{base + i*4:08x}', 'instructions': str(n)} for i, n in enumerate([3, 3, 3, 1, 1])]
