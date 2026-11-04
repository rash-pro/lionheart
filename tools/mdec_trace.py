"""Instrument a frozen host, then record/replay its MDEC/DMA observation oracle.

The private trace includes literal reads/writes, every hardware IRQ's CPU/device
cycles, and a complete RAM digest at each VBlank. No game data belongs in git.
Production sources and the recompiler are never instrumented in place.
"""
import argparse
import os
from pathlib import Path
import re
import shlex
import subprocess

ROOT = Path(__file__).resolve().parents[1]


def instrument_hw(source):
    replacements = {
        '#include "cycle_math.h"': '#include "cycle_math.h"\n'
        'void mdec_trace_observe(cpu_t *, unsigned, uint32_t, unsigned, uint32_t);',
        '    if (source >= 11) return;': '    if (source >= 11) return;\n'
        '    mdec_trace_observe(c, 2, source, 0, 0);',
        '    record(c, address, width, value, 0);': '    record(c, address, width, value, 0);\n'
        '    mdec_trace_observe(c, 0, address, width, value);',
        '    value &= mask;': '    value &= mask;\n'
        '    mdec_trace_observe(c, 1, address, width, value);',
    }
    for old, new in replacements.items():
        if source.count(old) != 1:
            raise ValueError(f'hardware instrumentation boundary changed: {old!r}')
        source = source.replace(old, new)
    return source


def build(args):
    host, output, runtime = args.host_build.resolve(), args.output.resolve(), args.runtime.resolve()
    output.mkdir(parents=True, exist_ok=False)
    flags = (host / 'CMakeFiles/ff8_runtime.dir/flags.make').read_text()
    options = []
    for key in ('C_DEFINES', 'C_INCLUDES', 'C_FLAGS'):
        match = re.search(r'^' + key + r' = (.*)$', flags, re.M)
        options += shlex.split(match[1])
    options = [flag for flag in options if not flag.startswith('-I')]
    options += ['-I' + str(runtime)]
    source = output / 'hw.c'
    source.write_text(instrument_hw((runtime / 'hw.c').read_text()))
    for name, path in [('hw', source), ('trace', Path(__file__).with_suffix('.c'))]:
        subprocess.run(['cc', *options, '-c', str(path), '-o', str(output / (name + '.o'))], check=True)
    command = shlex.split((host / 'CMakeFiles/ff8.dir/link.txt').read_text())
    # Preserve the original object's list and archive ordering. The staged hw.o
    # satisfies all hardware symbols before the old archive member is searched.
    command[command.index('-o') + 1] = str(output / 'ff8')
    first_archive = next(i for i, item in enumerate(command) if item.endswith('.a'))
    command[first_archive:first_archive] = [str(output / 'hw.o'), str(output / 'trace.o')]
    command = [item for item in command if not item.startswith('-Wl,--dependency-file=')]
    subprocess.run(command, cwd=host, check=True)
    print(output / 'ff8')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest='mode', required=True)
    builder = sub.add_parser('build')
    builder.add_argument('--host-build', type=Path, required=True)
    builder.add_argument('--runtime', type=Path, default=ROOT / 'runtime')
    builder.add_argument('--output', type=Path, required=True)
    for mode in ('record', 'replay'):
        runner = sub.add_parser(mode)
        runner.add_argument('--trace', type=Path, required=True)
        runner.add_argument('command', nargs=argparse.REMAINDER)
    args = parser.parse_args()
    if args.mode == 'build':
        build(args)
        return
    if not args.trace.resolve().is_relative_to(ROOT / 'build'):
        parser.error('disc-derived traces must stay under build/')
    command = args.command[1:] if args.command[:1] == ['--'] else args.command
    if not command:
        parser.error('supply the instrumented host command after --')
    if args.mode == 'record' and args.trace.exists():
        parser.error('use a new trace file')
    env = dict(os.environ)
    env.pop('FF8_MDEC_TRACE', None)
    env.pop('FF8_MDEC_REPLAY', None)
    env['FF8_MDEC_TRACE' if args.mode == 'record' else 'FF8_MDEC_REPLAY'] = str(args.trace.resolve())
    raise SystemExit(subprocess.run(command, env=env, cwd=ROOT).returncode)


if __name__ == '__main__':
    main()
