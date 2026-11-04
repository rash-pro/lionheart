"""Observe the span model against the original word calendar between deadlines."""
import ctypes as C
from pathlib import Path
import random
import subprocess

import pytest

from tools.mdec_trace import instrument_hw

ROOT = Path(__file__).resolve().parents[1]


@pytest.fixture(scope='module')
def models(tmp_path_factory):
    output = tmp_path_factory.mktemp('mdec-calendar')
    hardware = output / 'hw.c'
    hardware.write_text(instrument_hw((ROOT / 'runtime/hw.c').read_text()))
    libraries = []
    for enabled in (0, 1):
        binary = output / f'model-{enabled}.so'
        sources = [p for p in sorted((ROOT / 'runtime').glob('*.c')) if p.name != 'hw.c']
        subprocess.run(['cc', '-std=c99', '-O2', '-Wall', '-Wextra', '-Werror', '-shared', '-fPIC',
                        '-Wl,-Bsymbolic', f'-DFF8_MDEC_CALENDAR={enabled}', '-I' + str(ROOT / 'runtime'),
                        str(ROOT / 'tests/mdec_calendar_harness.c'), str(hardware), *map(str, sources),
                        '-o', str(binary)], check=True)
        lib = C.CDLL(str(binary))
        signatures = {
            'create': ([C.c_uint] * 3, C.c_void_p), 'destroy': ([C.c_void_p], None),
            'advance': ([C.c_void_p, C.c_uint64], None),
            'write': ([C.c_void_p, C.c_uint32, C.c_uint, C.c_uint32], None),
            'read': ([C.c_void_p, C.c_uint32, C.c_uint], C.c_uint32),
            'ram_write': ([C.c_void_p, C.c_uint32, C.c_uint32], None),
            'ram_read': ([C.c_void_p, C.c_uint32], C.c_uint32),
            'compare': ([C.c_void_p] * 2, C.c_int), 'spans': ([C.c_void_p], C.c_uint64),
            'spu_compare': ([C.c_void_p] * 2, C.c_int),
            'irq_count': ([], C.c_uint), 'irq': ([C.c_uint] * 2, C.c_uint64),
        }
        for name, (arguments, result) in signatures.items():
            function = getattr(lib, 'calendar_' + name)
            function.argtypes, function.restype = arguments, result
        libraries.append(lib)
    return libraries


@pytest.mark.parametrize('depth', range(4))
@pytest.mark.parametrize('layout', range(4))
@pytest.mark.parametrize('control', [0, 1])
def test_word_calendar_observations(models, depth, layout, control):
    cpus = [lib.calendar_create(depth, layout, control) for lib in models]
    rng = random.Random(1400 + depth * 16 + layout * 2 + control)

    def both(name, *args):
        return [getattr(lib, 'calendar_' + name)(cpu, *args) for lib, cpu in zip(models, cpus)]

    def compare():
        assert models[1].calendar_compare(*cpus) == 0
        assert models[0].calendar_irq_count() == models[1].calendar_irq_count()
        count = models[0].calendar_irq_count()
        histories = [[[lib.calendar_irq(i, j) for j in range(3)] for i in range(count)] for lib in models]
        assert histories[0] == histories[1]

    try:
        output_words = 600 * (8 if depth == 0 else 16) if depth < 2 else 100 * (192 if depth == 2 else 128)
        destination = 0x2000 if layout == 3 else 0x10000
        both('write', 0x1f801090, 4, destination)
        both('write', 0x1f801094, 4, 0x10000 | output_words)
        both('write', 0x1f801098, 4, 0x01000200)
        cycle = 0
        # Single-cycle observations cover exact before/on/after boundaries;
        # random long gaps exercise the lazy path and overlapping grants.
        for step in range(600):
            cycle += 1 if step < 80 else rng.randrange(1, 1300)
            both('advance', cycle)
            compare()
            for address in (0x1f801824, 0x1f801080, 0x1f801084, 0x1f801088,
                            0x1f801090, 0x1f801094, 0x1f801098, 0x1f8010f4):
                values = both('read', address, 4)
                assert values[0] == values[1], (step, cycle, hex(address), values)
            if step in (100, 200):
                # A source mutation must be observed at its real DMA read,
                # including when it invalidates the preview or crosses a fence.
                both('ram_write', 0x2000 + step * 4, 0xfe000409)
            if step == 120:
                # A competing DMA4 observes SPU state and CPU RAM mid-movie.
                both('write', 0x1f801da4, 2, 0x201)
                both('write', 0x1f801da6, 2, 0x200)
                both('write', 0x1f801daa, 2, 0xc060)
                both('write', 0x1f8010c0, 4, 0x2000)
                both('write', 0x1f8010c4, 4, 0x10020)
                both('write', 0x1f8010c8, 4, 0x01000201)
            if step % 25 == 0:
                values = both('read', 0x1f801dae, 2)
                assert values[0] == values[1]
                assert models[1].calendar_spu_compare(*cpus) == 0
            if step == 300:
                # Partial register writes and cancellation retain exact state.
                both('write', 0x1f8010f4, 1, 0)
                both('write', 0x1f801098, 4, 0)
            if step == 400:
                both('write', 0x1f801824, 4, 0xe0000000)
            compare()
        if layout == 0:
            assert models[1].calendar_spans(cpus[1]) > 0
    finally:
        both('destroy')
