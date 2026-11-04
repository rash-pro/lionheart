"""Play DMA grants must retain pixels, bus ownership and cancellation safety."""
from pathlib import Path
import ctypes as C
import subprocess

import pytest


@pytest.fixture(scope='module')
def play_media(tmp_path_factory):
    root = Path(__file__).resolve().parents[1]
    binary = tmp_path_factory.mktemp('play-media') / 'media'
    subprocess.run(['cc', '-std=c99', '-O2', '-Wall', '-Wextra', '-Werror',
                    '-DFF8_PLAY', '-Iruntime', 'tests/media_harness.c',
                    *map(str, sorted(Path('runtime').glob('*.c'))),
                    '-Wl,--wrap=spu_cd_input', '-o', str(binary)], cwd=root, check=True)
    return binary


@pytest.mark.parametrize('case', ['mdec', 'dma', 'movie-overlap', 'movie-priority',
                                  'movie-cancel', 'movie-late-gpu'])
def test_play_grants(play_media, case):
    subprocess.run([play_media, case], check=True, timeout=60)


@pytest.mark.parametrize('depth', [2, 3])
@pytest.mark.parametrize('observer', ['mdec', 'spu'])
def test_aliasing_input_output_keep_word_observations(tmp_path, depth, observer):
    root = Path(__file__).resolve().parents[1]
    models = []
    for play in (False, True):
        binary = tmp_path / f'alias-{play}.so'
        subprocess.run(['cc', '-std=c99', '-O2', '-Wall', '-Wextra', '-Werror',
                        '-shared', '-fPIC', '-Wl,-Bsymbolic',
                        *(['-DFF8_PLAY'] if play else []), '-Iruntime',
                        'tests/mdec_calendar_harness.c',
                        *map(str, sorted(Path('runtime').glob('*.c'))),
                        '-o', str(binary)], cwd=root, check=True)
        lib = C.CDLL(str(binary))
        for name, args, result in (
            ('create', [C.c_uint] * 3, C.c_void_p),
            ('write', [C.c_void_p, C.c_uint32, C.c_uint, C.c_uint32], None),
            ('advance', [C.c_void_p, C.c_uint64], None),
            ('compare', [C.c_void_p] * 2, C.c_int),
            ('destroy', [C.c_void_p], None),
        ):
            function = getattr(lib, 'calendar_' + name)
            function.argtypes, function.restype = args, result
        models.append(lib)
    cpus = [lib.calendar_create(depth, 3, 0) for lib in models]
    try:
        for lib, cpu in zip(models, cpus):
            # MDEC output or SPU DMA overwrites future compressed input.
            # Observations must retain the original interleaving.
            lib.calendar_write(cpu, 0x1f801090, 4, 0x2000 if observer == 'mdec' else 0x100000)
            lib.calendar_write(cpu, 0x1f801094, 4, 0x10000 | (19200 if depth == 2 else 12800))
            lib.calendar_write(cpu, 0x1f801098, 4, 0x01000200)
            if observer == 'spu':
                lib.calendar_write(cpu, 0x1f8010c0, 4, 0x2000)
                lib.calendar_write(cpu, 0x1f8010c4, 4, 0x10000 | 1024)
                lib.calendar_write(cpu, 0x1f8010c8, 4, 0x01000200)
        for cycle in range(0, 20000, 7):
            for lib, cpu in zip(models, cpus):
                lib.calendar_advance(cpu, cycle)
            assert models[1].calendar_compare(*cpus) == 0, cycle
    finally:
        for lib, cpu in zip(models, cpus):
            lib.calendar_destroy(cpu)
