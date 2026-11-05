"""Relink a Linux x86-64 CMake host with per-VBlank CPU/wall measurements.

The default probe only clocks frame boundaries and presentation. --subsystems
adds exclusive device timers and sampled host/guest PCs: use those binaries for
diagnosis, never the reported A/B timings. Set FF8_FRAME_TIMINGS to the output CSV.
Build and relink the baseline before changing source or rebuilding its objects.
"""
import argparse
import os
from pathlib import Path
import re
import shlex
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def relink(build, output, subsystems=False):
    build, output = Path(build).resolve(), Path(output).resolve()
    output.parent.mkdir(parents=True, exist_ok=True)
    cache = (build / 'CMakeCache.txt').read_text()
    flags = ['-DFF8_STATS'] if 'FF8_STATS:BOOL=ON' in cache else []
    source = (ROOT / 'tools/frame_probe.c').read_text()
    symbols = ['cpu_init', 'host_present', 'hw_destroy']
    if subsystems:
        flags.append('-DSUBSYSTEMS')
        symbols += ['gte_cmd', 'gte_read', 'gte_write', 'gpu_gp0', 'gpu_gp0_words', 'gpu_gp1', 'gpu_read',
                    'cdrom_event', 'dma_event', 'dma_sync', 'spu_advance', 'spu_read', 'spu_write',
                    'spu_dma_write', 'spu_dma_read']
        for name, params in re.findall(r'void (gte_\w+)\(([^;]+)\);', (ROOT / 'runtime/gte.h').read_text()):
            types = [re.sub(r'\b\w+$', '', p.strip()).strip() for p in params.split(',')]
            decl = ','.join(f'{t} a{i}' for i, t in enumerate(types))
            args = ','.join(f'a{i}' for i in range(len(types)))
            source += f'\nWRAP_VOID({name},1,({decl}),({args}))\n'
            symbols.append(name)
    command = shlex.split((build / 'CMakeFiles/ff8.dir/link.txt').read_text())
    command[command.index('-o') + 1] = str(output)
    with tempfile.TemporaryDirectory(prefix='frame-probe-', dir=output.parent) as temporary:
        src, obj = Path(temporary) / 'probe.c', Path(temporary) / 'probe.o'
        src.write_text(source)
        subprocess.run([os.environ.get('CC', 'cc'), '-O2', '-std=c99', '-I'+str(ROOT / 'runtime'),
                        *flags, '-c', str(src), '-o', str(obj)], check=True)
        command[1:1] = [str(obj), *[f'-Wl,--wrap={name}' for name in symbols]]
        subprocess.run(command, cwd=build, check=True)
    return output


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('build', type=Path)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--subsystems', action='store_true')
    args = parser.parse_args()
    print(relink(args.build, args.output, args.subsystems))
