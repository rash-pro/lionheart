"""GTE register/command vectors derived from psx-spx's hardware equations.

Reference: https://psx-spx.consoledev.net/geometrytransformationenginegte/
No emulator implementation is used as the expected-result oracle.
Optional external vectors: set FF8_GTE_VECTORS to a JSON object with a `tests`
array of {name, input: [64 uint32], opcode, output: [64 uint32]}. Registers
are written in order; opcode 0xffffffff means transfers only. A public MIT
corpus with this structure is JaCzekanski/ps1-tests, gte/test-all/tests.c:
https://github.com/JaCzekanski/ps1-tests/blob/master/gte/test-all/tests.c
Its LICENSE credits Copyright (c) 2020 Jakub Czekański. External data is not
bundled; normal tests are offline, hand-derived and independent of that corpus.
"""

import ctypes as C
import itertools
import json
import os
from pathlib import Path
import random
import subprocess

import pytest

ROOT = Path(__file__).resolve().parents[1]
COMMANDS = (1, 6, 12, 16, 17, 18, 19, 20, 22, 27, 28, 30, 32, 40, 41, 42, 45, 46, 48, 61, 62, 63)
IDENTITY = [[4096, 0, 0], [0, 4096, 0], [0, 0, 4096]]


def u32(n):
    return n & 0xFFFFFFFF


def s32(n):
    n &= 0xFFFFFFFF
    return n if n < 0x80000000 else n - 0x100000000


def signed16(n):
    n &= 65535
    return n if n < 32768 else n - 65536


def xy(x, y):
    return (x & 65535) | ((y & 65535) << 16)


def flags(bits):
    return bits | (0x80000000 if bits & 0x7F87E000 else 0)


class GTE:
    def __init__(self, lib, cpu):
        self.lib, self.cpu = lib, cpu

    def write(self, reg, value, control=False):
        self.lib.gte_write(self.cpu, reg, u32(value), control)

    def read(self, reg, control=False):
        return self.lib.gte_read(self.cpu, reg, control)

    def command(self, cmd, sf=1, lm=0, mx=0, v=0, cv=0):
        self.lib.gte_cmd(self.cpu, cmd | sf << 19 | lm << 10 | mx << 17 | v << 15 | cv << 13)

    def matrix(self, select, rows):
        flat = list(itertools.chain.from_iterable(rows)) + [0]
        for i in range(5):
            self.write(select * 8 + i, xy(*flat[2*i:2*i+2]), True)

    def vector(self, select, values):
        if select == 3:
            for reg, value in zip((9, 10, 11), values):
                self.write(reg, value)
        else:
            self.write(select * 2, xy(*values[:2]))
            self.write(select * 2 + 1, values[2])

    def triple(self, reg):
        return [s32(self.read(reg + i)) for i in range(3)]

    @property
    def flag(self):
        return self.read(31, True)


@pytest.fixture(scope="session", params=[()], ids=["host"])
def gte_lib(tmp_path_factory, request):
    directory = tmp_path_factory.mktemp("gte")
    output = directory / "gte.so"
    command = [os.environ.get("CC", "cc"), *request.param, "-std=c99", "-O2", "-g", "-Wall", "-Wextra",
               "-Wconversion", "-Werror", "-pedantic-errors", "-shared", "-fPIC",
               "-I", str(ROOT / "runtime"), str(ROOT / "runtime/gte.c"),
               str(ROOT / "tests/gte_harness.c"), "-o", str(output)]
    if os.environ.get("FF8_TEST_SANITIZE"):
        command += ["-fsanitize=undefined", "-fno-sanitize-recover=all"]
    subprocess.run(command, check=True, capture_output=True, text=True)
    lib = C.CDLL(str(output))
    lib.gte_test_new.restype = C.c_void_p
    for name in ("gte_test_free", "gte_test_reset", "gte_test_count"):
        getattr(lib, name).argtypes = [C.c_void_p]
    lib.gte_test_count.restype = C.c_uint64
    lib.gte_write.argtypes = [C.c_void_p, C.c_uint, C.c_uint32, C.c_int]
    lib.gte_write.restype = None
    lib.gte_read.argtypes = [C.c_void_p, C.c_uint, C.c_int]
    lib.gte_read.restype = C.c_uint32
    lib.gte_cmd.argtypes = [C.c_void_p, C.c_uint32]
    lib.gte_cmd.restype = None
    return lib


@pytest.fixture
def gte(gte_lib):
    cpu = gte_lib.gte_test_new()
    assert cpu
    try:
        yield GTE(gte_lib, cpu)
    finally:
        gte_lib.gte_test_free(cpu)


@pytest.mark.parametrize("control", [False, True])
@pytest.mark.parametrize("value", [0, 0x12348001, 0x89AB7FFF, 0xFFFFFFFF])
def test_register_widths(gte, control, value):
    signed = {4, 12, 20, 26, 27, 29, 30} if control else {1, 3, 5, 8, 9, 10, 11}
    unsigned = set() if control else {7, 16, 17, 18, 19}
    excluded = {31} if control else {15, 28, 29, 30, 31}
    for reg in set(range(32)) - excluded:
        gte.write(reg, value, control)
        expected = u32(signed16(value)) if reg in signed else value & 65535 if reg in unsigned else value
        assert gte.read(reg, control) == expected, reg
    assert gte.flag == 0


def test_xy_fifo_and_reserved_register(gte):
    for n in (11, 22, 33):
        gte.write(15, n)
    assert [gte.read(r) for r in range(12, 16)] == [11, 22, 33, 33]
    gte.write(14, 44)
    assert [gte.read(r) for r in range(12, 16)] == [11, 22, 44, 44]
    gte.write(23, 0xDEADBEEF)
    assert gte.read(23) == 0xDEADBEEF and gte.read(22) == 0


def test_color_conversion_and_read_only(gte):
    gte.write(28, 0xFFFF8000 | (3 << 10) | (2 << 5) | 1)
    assert gte.triple(9) == [128, 256, 384]
    assert gte.read(28) == gte.read(29) == 0xC41
    gte.vector(3, [-1, 4096, 127])
    assert gte.read(28) == gte.read(29) == 31 << 5
    gte.write(29, 0x7FFF)
    assert gte.triple(9) == [-1, 4096, 127] and gte.flag == 0


@pytest.mark.parametrize("count", range(33))
def test_leading_sign_bits(gte, count):
    value = 0 if count == 32 else 1 << (31-count)
    expected = max(1, count)
    for n in (value, u32(~value)):
        gte.write(30, n)
        assert gte.read(30) == n
        assert gte.read(31) == expected
        gte.write(31, 99)
        assert gte.read(31) == expected


@pytest.mark.parametrize("bit", range(32))
def test_flag_write_summary(gte, bit):
    gte.write(31, 1 << bit, True)
    assert gte.flag == flags((1 << bit) & 0x7FFFF000)


@pytest.mark.parametrize("cmd", COMMANDS)
def test_command_resets_flags_and_counts_once(gte, cmd):
    gte.write(31, 0xFFFFFFFF, True)
    gte.command(cmd)
    assert gte.flag == (0x80020000 if cmd in (1, 48) else 0)
    assert gte.lib.gte_test_count(gte.cpu) == 1


@pytest.mark.parametrize("h,z,quotient,overflow", [
    (0, 0, 0x1FFFF, True), (1, 0, 0x1FFFF, True), (0, 1, 0, False),
    (1, 1, 0x10000, False), (2, 1, 0x1FFFF, True), (1, 2, 0x8000, False),
    (0x8000, 0x8000, 0x10000, False), (0xFFFF, 0xFFFF, 0xFFFF, False),
    (0xFE3F, 0x7F20, 0x1FFFF, False), (0xF015, 0x780B, 0x1FFFF, False),
])
def test_unr_documented_edges(gte, h, z, quotient, overflow):
    # Translation supplies SZ3, DQA=1 exposes the quotient directly in MAC0.
    gte.write(7, z, True)
    gte.write(26, h, True)
    gte.write(27, 1, True)
    gte.command(1)
    assert gte.read(24) == quotient
    assert bool(gte.flag & (1 << 17)) == overflow


def test_unr_all_denominators(gte):
    # Independent integer formula generates seeds, exercising all 257 indices
    # and all normalization shifts; no access to the implementation's table.
    gte.write(27, 1, True)
    for z in range(1, 65536):
        h = (z * 12345) & 65535
        if h >= z * 2:
            expected, overflow = 0x1FFFF, True
        else:
            shift = 16 - z.bit_length()
            d = z << shift
            seed = max(0, (0x40000 // (((d - 0x7FC0) >> 7) + 256) + 1) // 2 - 257) + 257
            correction = (0x2000080 - d * seed) // 256
            reciprocal = (128 + correction * seed) // 256
            expected = min(0x1FFFF, ((h << shift) * reciprocal + 32768) // 65536)
            overflow = False
        gte.write(7, z, True)
        gte.write(26, h, True)
        gte.command(1)
        assert gte.read(24) == expected, (h, z)
        assert bool(gte.flag & (1 << 17)) == overflow, (h, z)


def test_rtps_saturation(gte):
    gte.matrix(0, IDENTITY)
    gte.vector(0, [2000, -2000, 100])
    gte.write(26, 100, True)
    gte.write(27, 256, True)
    gte.command(1)
    assert gte.triple(25) == [2000, -2000, 100]
    assert gte.triple(9) == [2000, -2000, 100]
    assert gte.read(14) == xy(1023, -1024)
    assert gte.read(19) == 100 and gte.read(8) == 4096
    assert gte.read(24) == 0x1000000
    assert gte.flag == 0x80006000


@pytest.mark.parametrize("sf,lm,z,mac,ir", [(0, 0, 10, 40960, 32767), (1, 1, -1, -1, 0)])
def test_rtps_ir3_flag_uses_unshifted_signed_depth(gte, sf, lm, z, mac, ir):
    gte.matrix(0, IDENTITY)
    gte.vector(0, [0, 0, z])
    gte.command(1, sf=sf, lm=lm)
    assert s32(gte.read(27)) == mac and gte.read(11) == ir
    assert not gte.flag & (1 << 22)
    assert gte.flag == (0x80060000 if z < 0 else 0)


def test_rtps_uses_full_mac0_before_saturation(gte):
    gte.matrix(0, IDENTITY)
    gte.vector(0, [32767, -32768, 1])
    gte.write(26, 2, True)
    gte.write(27, 32767, True)
    gte.command(1)
    assert gte.read(14) == xy(1023, -1024)
    assert gte.read(8) == 4096
    assert gte.read(24) == u32(0x1FFFF * 32767)
    assert gte.flag == flags((1 << 17) | (1 << 16) | (1 << 15) | (1 << 14) | (1 << 13) | (1 << 12))


def test_rtpt_fifo_and_final_depth_cue(gte):
    gte.matrix(0, IDENTITY)
    for i, vertex in enumerate(([10, 0, 100], [20, 0, 200], [30, 0, 400])):
        gte.vector(i, vertex)
    gte.write(19, 7)
    gte.write(26, 100, True)
    gte.write(27, 1, True)
    gte.command(0x30)
    assert [gte.read(i) for i in range(16, 20)] == [7, 100, 200, 400]
    assert [gte.read(i) for i in range(12, 15)] == [10, 10, 7]
    assert gte.triple(25) == [30, 0, 400]
    assert gte.read(24) == 16384 and gte.read(8) == 4 and gte.flag == 0


@pytest.mark.parametrize("mx,vs,cv,sf,lm", list(itertools.product(range(4), range(4), range(4), range(2), range(2))))
def test_mvmva_every_selection(gte, mx, vs, cv, sf, lm):
    matrices = [[[1 + k*9 + row*3 + col for col in range(3)] for row in range(3)] for k in range(3)]
    vectors = [[-3, 7, 11], [13, -17, 19], [-23, 29, -31], [37, -41, 43]]
    translations = [[2, -3, 4], [-5, 6, -7], [8, -9, 10], [0, 0, 0]]
    for k in range(3):
        gte.matrix(k, matrices[k])
        for i, n in enumerate(translations[k]):
            gte.write(5 + k*8 + i, n, True)
    for k in range(4):
        gte.vector(k, vectors[k])
    gte.write(6, 5)
    gte.write(8, 7)
    matrices.append([[-80, 80, 7], [3, 3, 3], [5, 5, 5]])
    expected, irs, errors = [], [], 0
    for i, row in enumerate(matrices[mx]):
        products = [a*b for a, b in zip(row, vectors[vs])]
        if cv == 2:
            discarded = (translations[cv][i]*4096 + products[0]) >> (sf*12)
            if not -32768 <= discarded <= 32767:
                errors |= 1 << (24-i)
            n = sum(products[1:]) >> (sf*12)
        else:
            n = (translations[cv][i]*4096 + sum(products)) >> (sf*12)
        ir = min(32767, max(0 if lm else -32768, n))
        if ir != n:
            errors |= 1 << (24-i)
        expected.append(n)
        irs.append(ir)
    gte.command(0x12, mx=mx, v=vs, cv=cv, sf=sf, lm=lm)
    assert gte.triple(25) == expected
    assert gte.triple(9) == irs
    assert gte.flag == flags(errors)


def test_mvmva_far_color_discarded_stage_still_flags(gte):
    gte.matrix(0, [[4096, 4096, 4096]] * 3)
    gte.vector(0, [10, 20, 30])
    for reg, value in zip((21, 22, 23), (40000, -40000, 0)):
        gte.write(reg, value, True)
    gte.command(0x12, cv=2, lm=1)
    assert gte.triple(25) == gte.triple(9) == [50, 50, 50]
    assert gte.flag == 0x81800000


@pytest.mark.parametrize("sf", [0, 1])
def test_matrix_intermediate_44_bit_wrap_latches_both_directions(gte, sf):
    # TRX is 4096 below +2^43; +8192 overflows, then -8192 underflows
    # the wrapped negative sum. The final numerical result alone hides both.
    gte.matrix(0, [[1, 1, 0], [0, 0, 0], [0, 0, 0]])
    gte.vector(0, [8192, -8192, 0])
    gte.write(5, 0x7FFFFFFF, True)
    gte.command(0x12, sf=sf)
    assert s32(gte.read(25)) == (0x7FFFFFFF if sf else -4096)
    assert s32(gte.read(9)) == (32767 if sf else -4096)
    assert gte.flag == flags((1 << 30) | (1 << 27) | ((1 << 24) if sf else 0))


@pytest.mark.parametrize("points,expected,error", [
    ([(0, 0), (10, 0), (0, 10)], 100, 0),
    ([(0, 0), (0, 10), (10, 0)], -100, 0),
    ([(0, 0), (1, 1), (2, 2)], 0, 0),
    ([(-32768, -32768), (32767, -32768), (-32768, 32767)], 65535**2, 1 << 16),
    ([(-32768, -32768), (-32768, 32767), (32767, -32768)], -(65535**2), 1 << 15),
])
def test_nclip_orientation_and_overflow(gte, points, expected, error):
    for i, point in enumerate(points):
        gte.write(12+i, xy(*point))
    gte.command(6)
    assert gte.read(24) == u32(expected) and gte.flag == flags(error)


@pytest.mark.parametrize("cmd,z,scale,expected,otz,error", [
    (0x2D, [999, 3, 6, 9], 1365, 24570, 5, 0),
    (0x2E, [4, 8, 12, 16], 1024, 40960, 10, 0),
    (0x2D, [0, 3, 6, 9], -1, -18, 0, 1 << 18),
    (0x2E, [65535]*4, 32767, 65535*4*32767, 65535, (1 << 18) | (1 << 16)),
    (0x2E, [65535]*4, -32768, -65535*4*32768, 0, (1 << 18) | (1 << 15)),
])
def test_avsz_full_sum_and_signed_scaling(gte, cmd, z, scale, expected, otz, error):
    for i, n in enumerate(z):
        gte.write(16+i, n)
    gte.write(29 if cmd == 0x2D else 30, scale, True)
    gte.command(cmd)
    assert gte.read(24) == u32(expected) and gte.read(7) == otz
    assert gte.flag == flags(error)


@pytest.mark.parametrize("sf,lm", list(itertools.product(range(2), range(2))))
def test_op_and_square(gte, sf, lm):
    gte.matrix(0, [[2, 99, 99], [99, 3, 99], [99, 99, 4]])
    gte.vector(3, [5, 6, 7])
    gte.command(0x0C, sf=sf, lm=lm)
    expected = [n >> (sf*12) for n in [-3, 6, -3]]
    assert gte.triple(25) == expected
    assert gte.triple(9) == [max(0, n) if lm else n for n in expected]
    assert gte.flag == flags((1 << 24 | 1 << 22) if lm else 0)
    gte.vector(3, [-32768, -3, 4])
    gte.command(0x28, sf=sf, lm=lm)
    expected = [n >> (sf*12) for n in [1073741824, 9, 16]]
    assert gte.triple(25) == expected
    assert gte.triple(9) == [min(32767, n) for n in expected]
    assert gte.flag == 0x81000000


@pytest.mark.parametrize("cmd,mac,rgb", [
    (0x1E, [256, 512, 768], 0xAA302010),
    (0x1B, [128, 128, 96], 0xAA060808),
    (0x13, [576, 1088, 1584], 0xAA634424),
    (0x1C, [128, 128, 96], 0xAA060808),
    (0x14, [576, 1088, 1584], 0xAA634424),
    (0x29, [576, 1088, 1584], 0xAA634424),
    (0x11, [640, 1280, 1920], 0xAA785028),
    (0x10, [1536, 1536, 1792], 0xAA706060),
])
def test_color_command_hand_vectors(gte, cmd, mac, rgb):
    gte.matrix(1, IDENTITY)
    gte.matrix(2, IDENTITY)
    gte.vector(0, [256, 512, 768])
    gte.vector(3, [256, 512, 768])
    gte.write(6, 0xAA204080)
    gte.write(8, 2048)
    for i, n in enumerate([1024, 2048, 3072]):
        gte.write(21+i, n, True)
    gte.write(21, 11)
    gte.write(22, 22)
    gte.command(cmd, lm=1)
    assert gte.triple(25) == gte.triple(9) == mac
    assert [gte.read(i) for i in (20, 21, 22)] == [11, 22, rgb]
    assert gte.flag == 0


@pytest.mark.parametrize("cmd,colors,last", [
    (0x20, [0xAA302010, 0xAA605040, 0xAA908070], [1792, 2048, 2304]),
    (0x3F, [0xAA060808, 0xAA0C1420, 0xAA122038], [896, 512, 288]),
    (0x16, [0xAA634424, 0xAA664A30, 0xAA69503C], [960, 1280, 1680]),
])
def test_triple_lighting(gte, cmd, colors, last):
    gte.matrix(1, IDENTITY)
    gte.matrix(2, IDENTITY)
    for i in range(3):
        gte.vector(i, [256+768*i, 512+768*i, 768+768*i])
        gte.write(21+i, [1024, 2048, 3072][i], True)
    gte.write(6, 0xAA204080)
    gte.write(8, 2048)
    gte.command(cmd, lm=1)
    assert [gte.read(i) for i in (20, 21, 22)] == colors
    assert gte.triple(25) == gte.triple(9) == last and gte.flag == 0


def test_dpct_reads_old_fifo_colors_and_rgbc_code(gte):
    colors = [0x11203040, 0x22506070, 0x338090A0]
    for i, n in enumerate(colors):
        gte.write(20+i, n)
    gte.write(6, 0xABFFFFFF)
    gte.command(0x2A)
    assert [gte.read(i) for i in (20, 21, 22)] == [0xAB000000 | (n & 0xFFFFFF) for n in colors]
    assert gte.triple(25) == [0xA00, 0x900, 0x800] and gte.flag == 0


@pytest.mark.parametrize("lm", [0, 1])
def test_depth_difference_always_has_signed_clamp(gte, lm):
    gte.vector(3, [4096, 4096, 4096])
    gte.write(8, 2048)
    gte.command(0x11, lm=lm)
    assert gte.triple(25) == gte.triple(9) == [2048]*3
    assert gte.flag == 0


def test_color_clamps_mac_not_ir(gte):
    gte.vector(3, [32767, -32768, 256])
    gte.write(8, 32767)
    gte.write(6, 0xAB000000)
    gte.command(0x3D, sf=0, lm=1)
    assert gte.read(22) == 0xABFF00FF
    assert gte.triple(9) == [32767, 0, 32767]
    assert gte.flag == 0x81F80000


@pytest.mark.parametrize("cmd,sf,lm", list(itertools.product([0x3D, 0x3E], range(2), range(2))))
def test_gpf_gpl_signed_input_and_base(gte, cmd, sf, lm):
    gte.vector(3, [64, -64, 32])
    gte.write(8, -16)
    old = [4096, 8192, 2048]
    for i, n in enumerate(old):
        gte.write(25+i, n)
    expected = [((v * -16) + (n << (sf*12) if cmd == 0x3E else 0)) >> (sf*12)
                for v, n in zip([64, -64, 32], old)]
    gte.command(cmd, sf=sf, lm=lm)
    assert gte.triple(25) == expected
    assert gte.triple(9) == [max(0, n) if lm else n for n in expected]
    assert gte.read(22) == sum(min(255, max(0, n >> 4)) << (8*i) for i, n in enumerate(expected))


def test_gpl_44_bit_overflow(gte):
    gte.vector(3, [32767, 0, 0])
    gte.write(8, 32767)
    gte.write(25, 0x7FFFFFFF)
    gte.command(0x3E)
    assert s32(gte.read(25)) == s32(0x7FFFFFFF + (32767**2 >> 12))
    assert s32(gte.read(9)) == -32768
    assert gte.flag == flags((1 << 30) | (1 << 24) | (1 << 21))


def test_deterministic_extreme_command_sequences(gte):
    # Exercises unusual sf/lm and ignored opcode bits under UBSan. The
    # invariants are externally visible widths/FLAG behavior, not a second
    # implementation of the command kernels.
    rng = random.Random(0x475445)
    extremes = [0, 1, 0xFFFFFFFF, 0x7FFFFFFF, 0x80000000, 0x7FFF7FFF, 0x80008000]
    for _ in range(200):
        for reg in range(64):
            gte.write(reg & 31, rng.choice(extremes) if rng.randrange(2) else rng.getrandbits(32), reg >= 32)
        for cmd in COMMANDS:
            gte.command(cmd, sf=rng.randrange(2), lm=rng.randrange(2), mx=rng.randrange(4),
                        v=rng.randrange(4), cv=rng.randrange(4))
            assert gte.flag == flags(gte.flag & 0x7FFFF000)
            assert all(-32768 <= n <= 32767 for n in gte.triple(9))
            assert gte.read(15) == gte.read(14)
            assert gte.read(28) == gte.read(29) <= 32767


def external_vectors():
    path = os.environ.get("FF8_GTE_VECTORS")
    return json.loads(Path(path).read_text())["tests"] if path else []


@pytest.mark.parametrize("case", external_vectors(), ids=lambda case: case["name"])
def test_public_vectors(gte, case):
    for reg, value in enumerate(case["input"]):
        gte.write(reg & 31, value, reg >= 32)
    if case["opcode"] != 0xFFFFFFFF:
        gte.lib.gte_cmd(gte.cpu, case["opcode"])
    actual = [gte.read(reg & 31, reg >= 32) for reg in range(64)]
    differences = {reg: (f"{a:08x}", f"{e:08x}") for reg, (a, e) in enumerate(zip(actual, case["output"])) if a != e}
    assert not differences, differences
