"""A disc swap must not fail because optional per-LBA diagnostics exhaust RAM."""
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[1]


def test_swap_survives_statistics_allocation_failure(tmp_path):
    output = tmp_path / 'swap'
    subprocess.run(['cc', '-std=c99', '-O1', '-DFF8_STATS', '-I', str(ROOT / 'runtime'),
                    str(ROOT / 'tests/cd_swap_memory_harness.c'),
                    *map(str, sorted((ROOT / 'runtime').glob('*.c'))),
                    '-Wl,--wrap=calloc', '-o', str(output)], check=True)
    images = [tmp_path / 'before.bin', tmp_path / 'after.bin']
    for image in images:
        with image.open('wb') as stream:
            stream.truncate(100001 * 2352)
    subprocess.run([str(output), *map(str, images)], check=True, timeout=20)
