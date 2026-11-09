#!/usr/bin/env python3
"""Prove the native test inputs and oracles equal the Python ones, while Python is here.

Run after building tests/native:   python tests/native/check_against_python.py

Each native case records its inputs and its oracle's answers (lh::equiv::record).
This script re-runs the real pytest bodies with the runtime replaced by a replay of
those records: the replay checks the inputs the pytest body generated are the ones
the native case used, and hands back the native oracle's answer, so the pytest
body's own `assert` compares the Python oracle against the native one on every
vector. Delete this file with Python, together with tests/native/support/equiv.h
and the lh::equiv::record calls; nothing else depends on them.
"""
import argparse
import ctypes
import hashlib
import importlib
import os
from pathlib import Path
import random
import re
import shutil
import struct
import subprocess
import sys
import tempfile
from types import SimpleNamespace
from unittest import mock

ROOT = Path(__file__).resolve().parents[2]
sys.path[:0] = [str(ROOT), str(ROOT / 'tests')]

FILES = ['test_spu_math', 'test_cycle_math', 'test_mdec_math', 'test_media',
         'test_media_play_grants', 'test_mdec_calendar', 'test_gpu']


class Records:
    def __init__(self, path):
        self.path = path
        self.data = path.read_bytes()
        self.at = 0

    def next(self, fmt):
        values = struct.unpack_from(fmt, self.data, self.at)
        self.at += struct.calcsize(fmt)
        return values

    def bytes(self, count):
        out = self.data[self.at:self.at + count]
        assert len(out) == count, f'{self.path}: expected {count} more bytes'
        self.at += count
        return out

    def done(self):
        assert self.at == len(self.data), f'{self.path}: {len(self.data) - self.at} recorded bytes were never compared'


def records(directory, case):
    path = directory / (re.sub(r'[^0-9A-Za-z]', '_', case) + '.bin')
    assert path.exists(), f'the native case {case} recorded nothing at {path}'
    return Records(path)


class Replay:
    """A ctypes-style function that checks arguments and answers from the records."""

    def __init__(self, stream, fmt, count):
        self.stream, self.fmt, self.count = stream, fmt, count

    def __call__(self, *args):
        row = self.stream.next(self.fmt)
        assert tuple(args) == row[:self.count], (args, row)
        return row[self.count]


def no_compile(*args, **kwargs):
    return SimpleNamespace(stdout=b'', returncode=0)


def check_spu_math(directory, tmp):
    module = importlib.import_module('test_spu_math')
    stream = records(directory, 'test_spu_math::test_adpcm_matches_integer_reference')

    def decoder(data, history, out):
        row = stream.next('<16B2h28h2h')
        assert bytes(data) == bytes(row[:16]), row
        assert list(history) == list(row[16:18]), row
        for i, value in enumerate(row[18:46]):
            out[i] = value
        history[0], history[1] = row[46], row[47]

    module.test_adpcm_matches_integer_reference(decoder)
    stream.done()

    stream = records(directory, 'test_spu_math::test_pcm_multiply_matches_wide_reference')
    library = SimpleNamespace(coefficient=Replay(stream, '<iii', 2), pcm=Replay(stream, '<iii', 2))
    with mock.patch.object(subprocess, 'run', no_compile), mock.patch.object(ctypes, 'CDLL', lambda path: library):
        module.test_pcm_multiply_matches_wide_reference(tmp)
    stream.done()


def check_cycle_math(directory, tmp):
    module = importlib.import_module('test_cycle_math')
    stream = records(directory, 'test_cycle_math::test_cycle_reciprocals')
    library = SimpleNamespace(ratio=Replay(stream, '<QIQ', 2), product=Replay(stream, '<QQQ', 2))
    with mock.patch.object(subprocess, 'run', no_compile), mock.patch.object(ctypes, 'CDLL', lambda path: library):
        module.test_cycle_reciprocals(tmp)
    stream.done()


def check_mdec_math(directory, tmp):
    module = importlib.import_module('test_mdec_math')
    stream = records(directory, 'test_mdec_math::test_idct_rounding_and_full_int16_bounds')

    def transform(values, scale):
        row = stream.next('<192h')
        assert list(values) == list(row[:64]) and list(scale) == list(row[64:128]), row[:128]
        for i in range(64):
            values[i] = row[128 + i]

    module.test_idct_rounding_and_full_int16_bounds(transform)
    stream.done()


def media_run(stream, expected_input, output):
    def run(command, input=None, stdout=None, check=None, timeout=None):
        assert input == expected_input(), command
        return SimpleNamespace(stdout=output(), returncode=0)
    return run


def check_media(directory, tmp):
    module = importlib.import_module('test_media')
    stream = records(directory, 'test_media::test_idct_fixed_point')

    def run(command, input=None, **kwargs):
        row = stream.next('<192h')
        assert input == struct.pack('<128h', *row[:128]), command
        return SimpleNamespace(stdout=struct.pack('<64h', *row[128:]), returncode=0)

    with mock.patch.object(subprocess, 'run', run):
        module.test_idct_fixed_point(Path('/nonexistent'))
    stream.done()

    for bits, stereo in [(4, False), (4, True), (8, False), (8, True)]:
        stream = records(directory, f'test_media::test_xa_predictor_arithmetic_and_history[{bits}-{stereo}]')
        raw = stream.bytes(2352)
        pcm = stream.data[2352:]

        def run_xa(command, input=None, **kwargs):
            assert input == raw and command[1:] == ['xa'], command
            return SimpleNamespace(stdout=pcm, returncode=0)

        with mock.patch.object(subprocess, 'run', run_xa):
            module.test_xa_predictor_arithmetic_and_history(Path('/nonexistent'), bits, stereo)
        stream.at = len(stream.data)
        stream.done()


def check_mdec_calendar(directory, tmp):
    module = importlib.import_module('test_mdec_calendar')
    for depth in range(4):
        for layout in range(4):
            for control in range(2):
                stream = records(directory, f'test_mdec_calendar::test_word_calendar_observations[{control}-{layout}-{depth}]')
                remembered = {}

                class Model:
                    def __init__(self, index):
                        self.index = index

                    def __getattr__(self, name):
                        return lambda *args: 0

                    def calendar_create(self, *args):
                        assert args == (depth, layout, control)
                        return 0

                    def calendar_spans(self, cpu):
                        return 1

                    def calendar_advance(self, cpu, cycle):
                        if self.index == 0:
                            remembered['cycle'] = stream.next('<Q')[0]
                        assert cycle == remembered['cycle'], (depth, layout, control, cycle)

                module.test_word_calendar_observations([Model(0), Model(1)], depth, layout, control)
                stream.done()


def check_gpu(directory, tmp):
    module = importlib.import_module('test_gpu')
    stream = records(directory, 'test_gpu::test_truncated_packets_and_random_control_streams')

    class Gpu:
        g = 0

        def __init__(self):
            self.pending = None
            self.lib = SimpleNamespace(gpu_read=lambda g: 0)

        def gp0(self, a, b):
            row = stream.next('<III')
            assert (a, b) == row[:2], row
            self.pending = row[2]

        def gp1(self, word):
            if word != 0x01000000:
                assert word == self.pending, (word, self.pending)

        def status(self):
            return 1 << 26

    module.test_truncated_packets_and_random_control_streams(Gpu())
    stream.done()

    png_cases = [f'test_gpu::test_present_pngs_and_vram_exit[{headless}]' for headless in ('True', 'False')]
    png_cases.append('test_gpu::test_zero_display_ranges_are_black_one_pixel_png')
    for case in png_cases:
        stream = records(directory, case)
        count = 0
        while stream.at < len(stream.data):
            size = stream.next('<I')[0]
            png = stream.bytes(size)
            width, height, rgb_size = stream.next('<III')
            rgb = stream.bytes(rgb_size)
            path = tmp / f'png-{count}.png'
            path.write_bytes(png)
            assert module.read_png(path) == (width, height, rgb), case
            count += 1
        assert count, f'{case}: no PNGs recorded'


def check_instrumented_hw(build):
    from tools.mdec_trace import instrument_hw
    generated = (build / 'generated/test_mdec_calendar/hw.c').read_bytes()
    assert generated == instrument_hw((ROOT / 'runtime/hw.c').read_text()).encode(), 'generated hw.c differs'


def check_py_random(directory):
    stream = records(directory, 'oracle_self_tests::py_random_vectors_for_the_python_check')
    for seed in [0, 1, 8, 90210, 0xadc0, 0x1820, 0x505358, 1400, 0xffffffff, 0x100000000,
                 0x100000007, 0xfffffffffffffffe]:
        r = random.Random(seed)
        for _ in range(16):
            assert stream.next('<d')[0] == r.random(), ('random', seed)
        for k in range(1, 65):
            assert stream.next('<Q')[0] == r.getrandbits(k), ('getrandbits', seed, k)
        for _ in range(64):
            row = stream.next('<qqqqqq')
            assert row == (r.randrange(1, 2), r.randrange(-98304, 98305), r.randrange(1, 1300),
                           r.randrange(65536), r.randint(-32768, 32767), r.randrange(65537)), ('randrange', seed)
        for size in [1, 3, 4, 5, 14, 16, 31, 32]:
            assert stream.bytes(size) == r.randbytes(size), ('randbytes', seed, size)
    stream.done()


def write_png_vectors(directory):
    import zlib
    vectors = directory / 'png_vectors'
    vectors.mkdir()
    rng = random.Random(0x706e67)
    for name, width, height, payload, level in [
        ('flat-stored', 4, 3, bytes([255, 0, 0]) * 12, 0),
        ('flat-compressed', 4, 3, bytes([255, 0, 0]) * 12, 9),
        ('pattern', 16, 16, bytes(i * 37 & 255 for i in range(16 * 16 * 3)), 6),
        ('random-stored', 32, 8, rng.randbytes(32 * 8 * 3), 0),
        ('random-compressed', 32, 8, rng.randbytes(32 * 8 * 3), 1),
    ]:
        raw = b''.join(b'\0' + payload[row * width * 3:(row + 1) * width * 3] for row in range(height))
        compressed = zlib.compress(raw, level)
        def chunk(kind, body):
            return struct.pack('>I', len(body)) + kind + body + struct.pack('>I', zlib.crc32(kind + body))
        png = b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', width, height, 8, 2, 0, 0, 0))
        png += chunk(b'IDAT', compressed) + chunk(b'IEND', b'')
        (vectors / f'{name}.png').write_bytes(png)
        (vectors / f'{name}.expected').write_text(f'{width} {height} {hashlib.sha256(payload).hexdigest()}')
    return vectors


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('build', nargs='?', type=Path, default=ROOT / 'build/native-tests')
    args = parser.parse_args()
    build = args.build.resolve()
    runner = build / 'lionheart_tests'
    if not runner.exists():
        parser.error(f'build tests/native first: {runner} is missing')
    with tempfile.TemporaryDirectory(prefix='lh-equiv-') as tmp_name:
        tmp = Path(tmp_name)
        directory = tmp / 'records'
        directory.mkdir()
        write_png_vectors(directory)
        cases = [line.split('\t')[1] for line in subprocess.run([runner, '--list'], check=True, text=True,
                                                                stdout=subprocess.PIPE).stdout.splitlines()]
        wanted = [c for c in cases if c.split('::')[0] in FILES or c.startswith('oracle_self_tests::')]
        environment = dict(os.environ, LH_EQUIV_DIR=str(directory))
        failed = [c for c in wanted if subprocess.run([runner, '--build-info', build / 'lh_build_info.txt', '--run', c],
                                                      env=environment, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                                      text=True, timeout=600).returncode != 0]
        assert not failed, f'native cases failed: {failed}'
        checks = [('generator: every method and seed against CPython', lambda: check_py_random(directory)),
                  ('instrumented hw.c is what tools/mdec_trace.py generates', lambda: check_instrumented_hw(build)),
                  ('test_spu_math: inputs and oracles', lambda: check_spu_math(directory, tmp)),
                  ('test_cycle_math: inputs and oracles', lambda: check_cycle_math(directory, tmp)),
                  ('test_mdec_math: inputs and oracle', lambda: check_mdec_math(directory, tmp)),
                  ('test_media: IDCT and XA inputs and oracles', lambda: check_media(directory, tmp)),
                  ('test_mdec_calendar: all random step sequences', lambda: check_mdec_calendar(directory, tmp)),
                  ('test_gpu: control stream and PNG oracle', lambda: check_gpu(directory, tmp))]
        for label, check in checks:
            check()
            print(f'equal  {label}')
    print('native inputs and oracles equal the Python ones')


if __name__ == '__main__':
    main()
