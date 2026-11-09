#!/usr/bin/env python3
"""Prove the synthetic programs native cases feed the C generator are the bytes pytest feeds Python.

Run after building tests/native:   python tests/native/check_generator_inputs.py

Native cases record every program they hand to lh::gen::Build. This script runs
the same files under pytest with discover() and emit() wrapped to record the same
facts, then compares them: per identity where a fixture emits for that test, and
per file for the shared fixtures that emit once. It also checks that each C probe
is the text of its Python string. Delete this file with Python, together with the
lh::equiv::record call in tests/native/support/generated.cpp; nothing else needs it.
"""
import argparse
import ast
import collections
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile

import pytest

ROOT = Path(__file__).resolve().parents[2]
sys.path[:0] = [str(ROOT), str(ROOT / 'tests')]

# Each ported file; test_recompiler is compared for its ported functions only.
FILES = ['test_scratchpad', 'test_idle', 'test_poll', 'test_poll_calendars',
         'test_recomp_configuration', 'test_recompiler', 'test_runtime', 'test_gte_timing']

# The C probes moved out of Python strings, in file order.
PROBES = {
    'test_gte_timing': ['test_gte_timing.c'],
    'test_scratchpad': ['test_scratchpad.c'],
    'test_idle': ['test_idle.c', 'test_idle_masked.c'],
    'test_poll': ['test_poll.c', 'test_poll_irq.c'],
    'test_poll_calendars': ['test_poll_calendars.c'],
    'test_recomp_configuration': ['test_recomp_configuration_boundary.c', 'test_recomp_configuration_slot.c',
                                  'test_recomp_configuration_dma.c', 'test_recomp_configuration_stats.c',
                                  'test_recomp_configuration_clock.c', 'test_recomp_configuration_irq.c'],
    'test_overlays': ['test_overlays_profile.c'],
}


def image_text(image, seeds, scan_calls):
    ranges = ''.join(f'{lo}-{hi},' for lo, hi in image.code_ranges)
    points = ''.join(f'{p},' for p in image.entry_points)
    seed_text = ''.join(f'{s},' for s in seeds)
    return f'{image.base}|{image.entry}|{ranges}|{points}|{seed_text}|{int(bool(scan_calls))}|{bytes(image.data).hex()}'


class Recorder:
    """Wraps discover and emit before the test modules import them."""

    def __init__(self):
        import recomp.discover
        import recomp.emit
        self.programs = collections.defaultdict(set)
        self.discoveries = {}
        self.real_discover = recomp.discover.discover
        self.real_emit = recomp.emit.emit
        recomp.discover.discover = self.discover
        recomp.emit.emit = self.emit

    def discover(self, image, seeds=(), scan_calls=True, *args, **kwargs):
        result = self.real_discover(image, seeds, scan_calls, *args, **kwargs)
        self.discoveries[id(result)] = (result, tuple(seeds), scan_calls)
        return result

    def emit(self, image, functions, output, mode='fast', overlays=(), host_mode='fast', timing='exact', register_cache=None):
        current = os.environ['PYTEST_CURRENT_TEST'].split(' ')[0]
        _, seeds, scan = self.discoveries[id(functions)]
        text = f'program|{mode}|{host_mode}|{timing}|{image_text(image, seeds, scan)}'
        for index, overlay, funcs in overlays:
            _, overlay_seeds, overlay_scan = self.discoveries[id(funcs)]
            text += f'|overlay|{index}|{image_text(overlay, overlay_seeds, overlay_scan)}'
        self.programs[current].add(text)
        return self.real_emit(image, functions, output, mode=mode, overlays=overlays, host_mode=host_mode,
                              timing=timing, register_cache=register_cache)


def native_name(node):
    return node.removeprefix('tests/').replace('.py::', '::', 1)


def pending_functions():
    source = (ROOT / 'tests/native/cases/test_recompiler.cpp').read_text()
    return set(re.findall(r'LH_PENDING\((\w+)\)', source))


def check_probes():
    for stem, names in PROBES.items():
        tree = ast.parse((ROOT / 'tests' / f'{stem}.py').read_text())
        strings = [node.value for node in sorted(ast.walk(tree), key=lambda n: getattr(n, 'lineno', 0))
                   if isinstance(node, ast.Constant) and isinstance(node.value, str) and '#include' in node.value]
        assert len(strings) == len(names), f'{stem}: {len(strings)} C strings, {len(names)} probes'
        for name, text in zip(names, strings):
            actual = (ROOT / 'tests/native/cases' / name).read_text()
            assert actual == text, f'{name} is not the text of its Python string'
    print(f'probes: {sum(len(v) for v in PROBES.values())} C files equal their Python strings')


def main():
    os.chdir(ROOT)
    parser = argparse.ArgumentParser()
    parser.add_argument('--build', type=Path, default=ROOT / 'build/native-tests')
    parser.add_argument('-j', type=int, default=6)
    parser.add_argument('--files', nargs='+', default=FILES, help='a subset of the ported files')
    options = parser.parse_args()

    check_probes()

    with tempfile.TemporaryDirectory(prefix='lionheart-inputs-') as temporary:
        records = Path(temporary) / 'records'
        records.mkdir()
        env = dict(os.environ, LH_EQUIV_DIR=str(records))
        pattern = '^(' + '|'.join(options.files) + ')::'
        subprocess.run(['ctest', '--test-dir', str(options.build), '-R', pattern, '-j', str(options.j),
                        '--output-on-failure'], env=env, check=True, stdout=subprocess.DEVNULL)
        native = collections.defaultdict(set)
        listing = subprocess.run([str(options.build / 'lionheart_tests'), '--list'], check=True, capture_output=True,
                                 text=True).stdout
        names = {re.sub(r'[^0-9A-Za-z]', '_', line.split('\t')[1]): line.split('\t')[1] for line in listing.splitlines()}
        for path in records.iterdir():
            case = names.get(path.stem)
            if case is None:
                raise SystemExit(f'unknown native record {path.name}')
            for line in path.read_text().splitlines():
                native[case].add(line)

    recorder = Recorder()
    selected = [f'tests/{stem}.py' for stem in options.files]
    status = pytest.main(['-q', '-x', '-p', 'no:cacheprovider', '--rootdir', str(ROOT), '-m', 'not game and not realtime', *selected],
                         plugins=[])
    if status != 0:
        raise SystemExit(f'pytest failed with status {status}')

    pending = pending_functions()
    failures = 0
    python_union, native_union = collections.defaultdict(set), collections.defaultdict(set)
    compared = 0
    for node, programs in recorder.programs.items():
        case = native_name(node)
        stem, _, rest = case.partition('::')
        python_union[stem] |= programs
        if rest.split('[')[0] in pending:
            continue
        if case not in native:
            # A session or module fixture emitted during a test this run skipped natively.
            continue
        compared += 1
        if native[case] != programs:
            failures += 1
            print(f'MISMATCH per identity: {case}')
    for case, programs in native.items():
        stem = case.split('::', 1)[0]
        native_union[stem] |= programs
    for stem in options.files:
        missing = python_union[stem] - native_union[stem]
        extra = native_union[stem] - python_union[stem]
        status = 'identical' if not missing and not extra else f'{len(missing)} Python-only, {len(extra)} native-only'
        if missing or extra:
            failures += 1
        print(f'{stem}: {len(python_union[stem])} distinct programs, {status}')
    print(f'{compared} identities compared one by one')
    if failures:
        raise SystemExit(1)
    print('all programs fed to the C generator are the bytes pytest feeds the Python generator')


if __name__ == '__main__':
    main()
