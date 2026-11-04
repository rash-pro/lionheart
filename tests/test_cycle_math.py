"""Exact timing reciprocals, including the high-word carry and correction cases."""
import ctypes
import random
import subprocess


def test_cycle_reciprocals(tmp_path):
    divisors = [263, 314, 526, 628, 62800, 63120000, 33868800, 33868800 * 1001]
    source = tmp_path / 'ratios.c'
    source.write_text('#include "cycle_math.h"\nuint64_t ratio(uint64_t n, unsigned i) { switch(i) {\n' +
        '\n'.join(f'case {i}: return CYCLE_DIV(n, UINT64_C({d}));' for i, d in enumerate(divisors)) +
        '\n} return 0; }\nuint64_t product(uint64_t a, uint64_t b) { return cycle_mul(a,b); }\n')
    binary = tmp_path / 'ratios.so'
    subprocess.run(['cc', '-O2', '-shared', '-fPIC', '-Iruntime', str(source), '-o', str(binary)], check=True)
    lib = ctypes.CDLL(str(binary))
    lib.ratio.argtypes = [ctypes.c_uint64, ctypes.c_uint]
    lib.ratio.restype = ctypes.c_uint64
    lib.product.argtypes = [ctypes.c_uint64, ctypes.c_uint64]
    lib.product.restype = ctypes.c_uint64
    rng = random.Random(8)
    mask = (1 << 64) - 1
    for i, d in enumerate(divisors):
        values = [0, 1, d-1, d, d+1, mask, mask-1, (1 << 32)-1, 1 << 32]
        for k in [1, 2, 65535, (1 << 32)-1, mask // d]:
            values.extend(n for n in (k*d-1, k*d, k*d+1) if 0 <= n <= mask)
        values.extend(rng.getrandbits(64) for _ in range(10000))
        for n in values:
            assert lib.ratio(n, i) == n // d, (n, d)
    for _ in range(10000):
        a, b = rng.getrandbits(64), rng.getrandbits(64)
        assert lib.product(a, b) == (a * b) & mask
