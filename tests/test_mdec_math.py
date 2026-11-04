"""Compare the complete integer transform with an independent wide oracle."""
import ctypes
from pathlib import Path
import random
import subprocess

import pytest


def reference(block, scale):
    for _ in range(2):
        block = [max(-32768, min(32767, (sum(block[row + k * 8] * (scale[x + k * 8] // 8)
                    for k in range(8)) + 0x4000) // 32768)) for row in range(8) for x in range(8)]
    signed = [((value & 511) ^ 256) - 256 for value in block]
    return [max(-128, min(127, value)) for value in signed]


@pytest.fixture(scope='module')
def transform(tmp_path_factory):
    root = Path(__file__).resolve().parents[1]
    output = tmp_path_factory.mktemp('mdec-math') / 'mdec.so'
    subprocess.run(['cc', '-std=c99', '-O2', '-shared', '-fPIC', '-I' + str(root / 'runtime'),
                    str(root / 'runtime/mdec.c'), '-o', str(output)], check=True, capture_output=True)
    library = ctypes.CDLL(str(output))
    library.mdec_idct.argtypes = [ctypes.POINTER(ctypes.c_int16), ctypes.POINTER(ctypes.c_int16)]
    return library.mdec_idct


def test_idct_rounding_and_full_int16_bounds(transform):
    rng = random.Random(90210)
    extremes = [-32768, -32767, -1, 0, 1, 32766, 32767]
    vectors = [(b, s) for b in ([v] * 64 for v in extremes) for s in ([v] * 64 for v in extremes)]
    for _ in range(256):
        vectors.append(([rng.randint(-32768, 32767) for _ in range(64)],
                        [rng.randint(-32768, 32767) for _ in range(64)]))
    for _ in range(256):
        # Sparse coefficient blocks exercise the zero-column shortcut in both passes.
        block = [rng.randint(-32768, 32767) if rng.random() < 0.1 else 0 for _ in range(64)]
        vectors.append((block, [rng.randint(-32768, 32767) for _ in range(64)]))
    for block, scale in vectors:
        values = (ctypes.c_int16 * 64)(*block)
        transform(values, (ctypes.c_int16 * 64)(*scale))
        assert list(values) == reference(block, scale)
