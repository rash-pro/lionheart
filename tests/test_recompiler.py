import ctypes as C
import os
import random
import struct
import subprocess
import tempfile
from pathlib import Path

import pytest

from recomp.decode import PRIMARY, SPECIAL, decode
from recomp.discover import Image, discover
from recomp.emit import emit

ROOT = Path(__file__).resolve().parents[1]
BASE = 0x80010000
DATA = 0x80100000


def r(fn, rs=0, rt=0, rd=0, sh=0):
    return rs << 21 | rt << 16 | rd << 11 | sh << 6 | fn


def imm(op, rs, rt, value):
    return op << 26 | rs << 21 | rt << 16 | (value & 65535)


def j(op, target):
    return op << 26 | ((target >> 2) & 0x3FFFFFF)


def cop(op, rs, rt, rd):
    return op << 26 | rs << 21 | rt << 16 | rd << 11


RET = [r(8, rs=31), 0]
CASES = {}


def case(name, words):
    CASES[name] = words


case("load_delay", [imm(35, 20, 8, 0), r(33, 8, 0, 9), r(33, 8, 0, 10)] + RET)
case("load_overwrite", [imm(35, 20, 8, 0), imm(9, 0, 8, 99)] + RET)
case("load_twice", [imm(35, 20, 8, 0), imm(35, 20, 8, 4), r(33, 8, 0, 9), r(33, 8, 0, 10)] + RET)
case("load_zero", [imm(35, 20, 0, 0), r(33, 0, 0, 9)] + RET)
case("branch_delay", [imm(4, 8, 9, 2), imm(9, 0, 8, 17), imm(9, 0, 10, 22)] + RET)
case("load_branch", [imm(35, 20, 8, 0), imm(4, 8, 9, 2), r(33, 8, 0, 10), imm(9, 0, 11, 22)] + RET)
case("load_in_slot", [imm(4, 0, 0, 1), imm(35, 20, 8, 0), r(33, 8, 0, 9), r(33, 8, 0, 10)] + RET)
case("return_load", [r(8, rs=31), imm(35, 20, 8, 0)])
case("load_slot_taken", [imm(4, 8, 8, 3), imm(35, 20, 8, 0), r(33, 8, 0, 9), 0,
                         r(33, 8, 0, 10), r(33, 8, 0, 11)] + RET)
case("load_slot_fallthrough", [imm(5, 8, 8, 3), imm(35, 20, 8, 0), r(33, 8, 0, 9), 0,
                               r(33, 8, 0, 10)] + RET)
case("load_slot_overwrite", [imm(4, 0, 0, 1), imm(35, 20, 8, 0), imm(9, 0, 8, 77)] + RET)
case("load_slot_twice", [imm(4, 0, 0, 1), imm(35, 20, 8, 0), imm(35, 20, 8, 4),
                        r(33, 8, 0, 9), r(33, 8, 0, 10)] + RET)
case("pending_fault", [imm(35, 20, 8, 0), imm(35, 20, 8, 1)] + RET)
case("pending_overflow", [imm(35, 20, 8, 0), r(32, 9, 10, 8)] + RET)
case("isolation", [cop(16, 4, 8, 12), imm(43, 20, 9, 0), imm(35, 20, 10, 0), 0,
                   cop(16, 4, 0, 12), imm(43, 20, 9, 4), imm(35, 20, 11, 4), 0] + RET)
case("pending_syscall", [imm(35, 20, 8, 0), 12] + RET)
case("load_merge_address", [imm(35, 20, 8, 0), imm(34, 8, 8, 3), 0] + RET)

case("jr_capture", [r(8, rs=31), imm(9, 0, 31, 123)])
case("load_return", [imm(35, 20, 31, 0), r(8, rs=31), 0])
case("loop", [imm(9, 8, 8, -1), imm(5, 8, 0, -2), imm(9, 9, 9, 1)] + RET)
case("div", [r(26, 8, 9), r(18, rd=10), r(16, rd=11)] + RET)
case("divu", [r(27, 8, 9), r(18, rd=10), r(16, rd=11)] + RET)
case("mult", [r(24, 8, 9), r(18, rd=10), r(16, rd=11)] + RET)
case("multu", [r(25, 8, 9), r(18, rd=10), r(16, rd=11)] + RET)
case("hilo", [r(17, 8), r(19, 9), r(16, rd=10), r(18, rd=11)] + RET)
case("memory", [imm(40, 20, 8, 8), imm(41, 20, 9, 10), imm(43, 20, 10, 12),
                imm(32, 20, 11, 8), imm(36, 20, 12, 8), imm(33, 20, 13, 10),
                imm(37, 20, 14, 10), imm(35, 20, 15, 12), 0] + RET)
case("overflow_add", [r(32, 8, 9, 10)] + RET)
case("overflow_sub", [r(34, 8, 9, 10)] + RET)
case("overflow_addi", [imm(8, 8, 10, 1)] + RET)
case("misaligned_load", [imm(35, 20, 8, 1)] + RET)
case("misaligned_store", [imm(41, 20, 8, 1)] + RET)
case("exception_slot", [imm(4, 0, 0, 1), imm(35, 20, 8, 1)] + RET)
case("break", [13])
case("reserved", [0xFC000000])
case("cop_unusable", [17 << 26])
case("syscall", [12] + RET)
case("cop0", [cop(16, 4, 8, 12), cop(16, 0, 9, 12), r(33, 9, 0, 10),
              r(33, 9, 0, 11), 0x42000010] + RET)
case("gte", [cop(18, 4, 8, 6), cop(18, 6, 9, 7), cop(18, 0, 10, 6),
             r(33, 10, 0, 11), cop(18, 2, 12, 7), 0, imm(50, 20, 5, 0),
             imm(58, 20, 5, 4), 0x4A000001] + RET)
# A pending load must not replace the operand of a GTE transfer.
for control in range(2):
    for reg in range(32):
        case(f"gte_transfer_{control}_{reg}",
             [imm(35, 20, 8, 0), cop(18, 6 if control else 4, 8, reg),
              cop(18, 2 if control else 0, 9, reg), r(33, 9, 0, 10),
              r(33, 9, 0, 11)] + RET)

# GTE commands preserve instruction retirement, pending loads, and FLAG updates.
for command in (1, 6, 12, 16, 17, 18, 19, 20, 22, 27, 28, 30, 32, 40, 41, 42, 45, 46, 48, 61, 62, 63, 0, 58):
    for sf in range(2):
        for lm in range(2):
            word = 0x4A000000 | command | sf << 19 | lm << 10 | 0x7E000
            case(f"gte_command_{command}_{sf}_{lm}",
                 [cop(18, 6, 9, 31), cop(18, 4, 8, 9), cop(18, 4, 9, 10),
                  cop(18, 4, 8, 11), word, cop(18, 2, 10, 31),
                  r(33, 10, 0, 11), r(33, 10, 0, 12)] + RET)

for offset in range(4):
    case(f"lwl_{offset}", [imm(34, 20, 8, offset), 0] + RET)
    case(f"lwr_{offset}", [imm(38, 20, 8, offset), 0] + RET)
    case(f"unaligned_{offset}", [imm(34, 20, 8, offset + 3), imm(38, 20, 8, offset),
                               r(33, 8, 0, 9), r(33, 8, 0, 10)] + RET)
    case(f"unaligned_reverse_{offset}", [imm(38, 20, 8, offset), imm(34, 20, 8, offset + 3), 0] + RET)
    case(f"swl_{offset}", [imm(42, 20, 8, offset)] + RET)
    case(f"swr_{offset}", [imm(46, 20, 8, offset)] + RET)
for op in (4, 5, 6, 7):
    case(f"branch_{op}", [imm(op, 8, 9 if op < 6 else 0, 2), imm(9, 0, 10, 10), imm(9, 0, 11, 11)] + RET)
for rt in (0, 1):
    case(f"regimm_{rt}", [imm(1, 8, rt, 2), imm(9, 0, 10, 10), imm(9, 0, 11, 11)] + RET)

# Calls are assigned absolute addresses after the case list has been laid out.
case("callee", [r(33, 8, 0, 9), r(33, 8, 0, 10)] + RET)
case("call_direct", [])
case("call_indirect", [])
case("callee_return_load", [])
case("call_return_load", [])
case("incoming_load_twice", [])
case("call_alt_link", [])
case("link_branch", [])
case("link_branch_negative", [])
case("tail_jump", [])
case("fallback", [])
case("fallback_entry", [])
case("jump_table", [])
case("self_modify", [imm(43, 20, 8, 0), imm(9, 0, 9, 1)] + RET)
case('gte_self_modify', [cop(18, 4, 8, 2), imm(58, 20, 2, 0), imm(9, 0, 9, 1)] + RET)
for name in ('deep_recursion', 'nonlocal_loop', 'nonlocal_child', 'unexpected_child',
             'bios_longjmp', 'longjmp_child'):
    case(name, [])

RNG = random.Random(0xFF8)
for index in range(64):
    words = []
    for _ in range(40):
        rs, rt, rd = [RNG.randrange(1, 19) for _ in range(3)]
        if RNG.randrange(2):
            fn = RNG.choice([0, 2, 3, 4, 6, 7, 33, 35, 36, 37, 38, 39, 42, 43])
            words.append(r(fn, rs, rt, rd, RNG.randrange(32) if fn in (0, 2, 3) else 0))
        else:
            words.append(imm(RNG.choice([9, 10, 11, 12, 13, 14, 15]), rs, rt, RNG.randrange(65536)))
    case(f"random_{index}", words + RET)

for index in range(16):
    words = []
    for _ in range(32):
        rt = RNG.randrange(8, 13)
        if RNG.randrange(3):
            op = RNG.choice([32, 33, 34, 35, 36, 37, 38, 40, 41, 43])
            offset = RNG.randrange(16)
            if op in (33, 37, 41): offset &= ~1
            if op in (35, 43): offset &= ~3
            words.append(imm(op, 20, rt, offset))
        else:
            words.append(r(33, rt, RNG.randrange(8, 13), RNG.randrange(8, 13)))
    case(f'memory_random_{index}', words + RET)

PCS = {name: BASE + index * 0x200 for index, name in enumerate(CASES)}
CASES["call_direct"] = [r(33, 31, 0, 30), j(3, PCS['callee']), imm(35, 20, 8, 0), r(33, 30, 0, 31)] + RET
CASES['callee_return_load'] = [r(8, rs=31), imm(35, 20, 8, 0)]
CASES['call_return_load'] = [r(33, 31, 0, 30), j(3, PCS['callee_return_load']), 0, r(33, 8, 0, 9), r(33, 8, 0, 10), r(33, 30, 0, 31)] + RET
CASES['incoming_load_twice'] = [imm(35, 20, 8, 4), r(33, 8, 0, 9), r(33, 8, 0, 10)] + RET
CASES["call_indirect"] = [r(33, 31, 0, 30), r(9, 21, rd=31), imm(35, 20, 8, 0), r(33, 30, 0, 31)] + RET
CASES["call_alt_link"] = [r(9, 21, rd=22), imm(9, 0, 8, 66)] + RET
for name, reg in (("link_branch", 17), ("link_branch_negative", 16)):
    pc = PCS[name]
    CASES[name] = [r(33, 31, 0, 30), imm(1, 8, reg, (PCS['callee'] - (pc + 8)) // 4),
                   imm(9, 0, 12, 77), r(33, 30, 0, 31)] + RET
CASES['tail_jump'] = [j(2, PCS['callee']), imm(9, 0, 8, 55)]
CASES['fallback'] = [r(8, 21), imm(35, 20, 8, 0)]
CASES['fallback_entry'] = [j(2, PCS['callee']), r(33, 8, 0, 12)]
table_pc = PCS['jump_table']
table = table_pc + 0x80
CASES['jump_table'] = [imm(15, 0, 8, (table + 0x8000) >> 16), imm(9, 8, 8, table & 65535),
                      r(0, rt=4, rd=9, sh=2), r(33, 8, 9, 8), imm(35, 8, 8, 0),
                      0, r(8, 8), 0, imm(9, 0, 2, 42)] + RET + [imm(9, 0, 2, 77)] + RET
pc = PCS['deep_recursion']
CASES['deep_recursion'] = [imm(9, 29, 29, -8), imm(43, 29, 31, 0), imm(9, 8, 8, -1),
                          imm(6, 8, 0, 3), 0, j(3, pc), 0, imm(35, 29, 31, 0),
                          imm(9, 29, 29, 8)] + RET
pc = PCS['nonlocal_loop']
CASES['nonlocal_loop'] = [r(33, 31, 0, 30), j(3, PCS['nonlocal_child']), 0,
                         imm(9, 8, 8, -1), imm(7, 8, 0, -4), 0, r(33, 30, 0, 31)] + RET
CASES['nonlocal_child'] = [r(8, rs=21), 0]
CASES['unexpected_child'] = [r(33, 21, 0, 31)] + RET
CASES['bios_longjmp'] = [j(3, PCS['longjmp_child']), 0, imm(9, 0, 10, 77), r(33, 30, 0, 31)] + RET
CASES['longjmp_child'] = [imm(9, 0, 9, 0x14), j(3, 0xa0), 0, 13]


@pytest.fixture(scope="session", params=["trace", "fast", "compact", "precise"])
def compiled(request):
    data = bytearray(len(CASES) * 0x200)
    for name, words in CASES.items():
        struct.pack_into(f"<{len(words)}I", data, PCS[name] - BASE, *words)
    struct.pack_into("<2I", data, table - BASE, table_pc + 32, table_pc + 44)
    image = Image(bytes(data), BASE, BASE)
    functions = discover(image, [pc for name, pc in PCS.items() if name != 'fallback_entry'])
    out_root = ROOT / 'build/gen-recomp/tests'
    out_root.mkdir(parents=True, exist_ok=True)
    out = Path(tempfile.mkdtemp(dir=out_root))
    overlays = []
    for index in (0, 1, 32, 64):
        words = [r(33, 31, 0, 25), j(3, PCS['callee']), 0, imm(9, 0, 2, 42 + index % 32),
                 r(33, 25, 0, 31), *RET]
        blob = struct.pack(f'<{len(words)}I', *words) + bytes(16)
        overlay = Image(blob, 0x80098000, 0x80098000)
        overlays.append((index, overlay, discover(overlay)))
    manifest = emit(image, functions, out, mode='fast' if request.param in ('compact', 'precise') else request.param,
                    overlays=overlays, host_mode='precise' if request.param in ('precise', 'compact') else 'fast')
    command = [os.environ.get('FF8_TEST_CC', 'gcc'), '-std=c99', '-DFF8_STATS', '-O1', '-g', '-shared', '-fPIC', '-Wall', '-Wextra',
               '-Wno-unused-variable', '-Wno-unused-label', '-Wno-tautological-compare',
               '-I', str(ROOT / 'runtime'), '-I', str(out), str(ROOT / 'tests/harness.c')]
    if request.param == 'compact': command.append('-DFF8_COMPACT')
    if os.environ.get('FF8_TEST_SANITIZE'):
        command += ['-fsanitize=undefined', '-fno-sanitize-recover=all']
    support = Path(os.environ.get('FF8_TEST_SUPPORT', ROOT / 'runtime'))
    if support == ROOT / 'runtime': command.append('-DFF8_TEST_HARDWARE')
    command += [str(ROOT / 'runtime' / name) for name in ('cpu.c', 'interp.c')]
    command += [str(p) for p in sorted(support.glob('*.c')) if p.name not in {'cpu.c', 'interp.c'}]
    command += [str(p) for p in sorted(out.glob('*.c'))] + ['-o', str(out / 'test.so')]
    subprocess.run(command, check=True, capture_output=True, text=True)
    lib = C.CDLL(str(out / 'test.so'))
    lib.test_new.restype = C.c_void_p
    signatures = {
        'test_free': [C.c_void_p], 'test_register': [C.c_void_p, C.c_uint, C.c_uint32],
        'test_registry_count': [C.c_void_p, C.c_uint, C.c_size_t],
        'test_write': [C.c_void_p, C.c_uint32, C.c_uint32, C.c_uint],
        'test_read': [C.c_void_p, C.c_uint32, C.c_uint],
        'test_run': [C.c_void_p, C.c_uint32, C.c_uint],
        'test_run_legacy': [C.c_void_p, C.c_uint32],
        'test_limits': [C.c_void_p, C.c_uint64, C.c_uint64],
        'test_image': [C.c_void_p, C.c_uint32, C.c_void_p, C.c_size_t],
        'test_snapshot': [C.c_void_p, C.POINTER(C.c_uint32)],
        'test_count': [C.c_void_p, C.c_uint], 'test_reason': [C.c_void_p],
        'test_ram': [C.c_void_p], 'test_bios': [C.c_void_p, C.c_uint],
        'test_syscall': [C.c_void_p], 'test_step': [C.c_void_p, C.c_uint32],
        'test_gte_write': [C.c_void_p, C.c_uint, C.c_uint32, C.c_int],
        'test_gte_read': [C.c_void_p, C.c_uint, C.c_int],
        'test_poll': [C.c_void_p, C.c_uint32, C.c_uint64],
    }
    for name, args in signatures.items():
        getattr(lib, name).argtypes = args
    lib.test_read.restype = C.c_uint32
    lib.test_count.restype = C.c_uint64
    lib.test_code_epoch.argtypes = [C.c_void_p]
    lib.test_code_epoch.restype = C.c_uint64
    lib.test_reason.restype = C.c_char_p
    lib.test_ram.restype = C.c_void_p
    lib.test_gte_read.restype = C.c_uint32
    lib.test_logger.argtypes = [C.c_void_p, C.CFUNCTYPE(None, C.c_void_p, C.c_char_p)]
    return lib, bytes(data), manifest


def run(compiled, name, registers=None, memory=None, interpret=0, start=None):
    lib, data, _ = compiled
    c = lib.test_new()
    assert c
    try:
        lib.test_image(c, BASE, data, len(data))
        initial = {8: 0xAABBCCDD, 9: 0x11111111, 20: DATA, 21: PCS['callee']}
        initial.update(registers or {})
        for reg, value in initial.items():
            lib.test_register(c, reg, value & 0xFFFFFFFF)
        mem = {DATA: 0x44332211, DATA + 4: 0x88776655}
        mem.update(memory or {})
        for address, value in mem.items():
            lib.test_write(c, address, value, 4)
        lib.test_run(c, start or PCS[name], interpret)
        snapshot = (C.c_uint32 * 135)()
        lib.test_snapshot(c, snapshot)
        return list(snapshot), C.string_at(lib.test_ram(c), 0x200000 + 1024 + 65536), [lib.test_count(c, n) for n in range(6)], lib.test_reason(c)
    finally:
        lib.test_free(c)


def diff(compiled, name, registers=None, memory=None, start=None):
    a = run(compiled, name, registers, memory, start=start)
    b = run(compiled, name, registers, memory, interpret=1, start=start)
    assert a[0] == b[0], [(i, hex(x), hex(y)) for i, (x, y) in enumerate(zip(a[0], b[0])) if x != y]
    assert a[1] == b[1]
    assert a[3] == b[3]
    assert a[2][0] + a[2][4] == b[2][0]
    assert a[2][3] == b[2][3]
    assert a[2][5] == b[2][5]
    return a


@pytest.mark.parametrize('name', [n for n in CASES if n not in {
    'loop', 'call_alt_link', 'fallback', 'fallback_entry', 'deep_recursion',
    'nonlocal_loop', 'nonlocal_child', 'unexpected_child', 'bios_longjmp', 'longjmp_child'}])
def test_differential(compiled, name):
    diff(compiled, name, {4: 1, 8: 5, 9: 5} if name in {'syscall', 'pending_syscall'} else None)


def test_load_delay_expected(compiled):
    state, *_ = diff(compiled, 'load_delay')
    assert state[9:11] == [0xAABBCCDD, 0x44332211]
    state, *_ = diff(compiled, 'load_overwrite')
    assert state[8] == 99
    state, *_ = diff(compiled, 'load_twice')
    assert state[9:11] == [0xAABBCCDD, 0x88776655]
    state, *_ = diff(compiled, 'load_in_slot')
    assert state[9:11] == [0xAABBCCDD, 0x44332211]


@pytest.mark.parametrize('a,b', [(0, 0), (1, 0), (0xFFFFFFFF, 0), (0x80000000, 0),
                               (0x80000000, 0xFFFFFFFF), (0xFFFFFFF9, 3), (7, 0xFFFFFFFD), (0xFFFFFFFF, 7)])
@pytest.mark.parametrize('name', ['div', 'divu', 'mult', 'multu'])
def test_hilo_edges(compiled, name, a, b):
    state, *_ = diff(compiled, name, {8: a, 9: b})
    signed = name in {'div', 'mult'}
    x = a - (1 << 32) if signed and a >> 31 else a
    y = b - (1 << 32) if signed and b >> 31 else b
    if name.startswith('mult'):
        product = (x * y) & ((1 << 64) - 1)
        hi, lo = product >> 32, product & 0xFFFFFFFF
    elif not y:
        hi, lo = a, 1 if signed and x < 0 else 0xFFFFFFFF
    else:
        quotient = (abs(x) // abs(y)) * (-1 if (x < 0) != (y < 0) else 1)
        hi, lo = (x - quotient * y) & 0xFFFFFFFF, quotient & 0xFFFFFFFF
    assert state[32:34] == [hi, lo]


@pytest.mark.parametrize('offset', range(4))
def test_unaligned_expected(compiled, offset):
    state, *_ = diff(compiled, f'unaligned_{offset}')
    expected = int.from_bytes(bytes.fromhex('1122334455667788')[offset:offset + 4], 'little')
    assert state[10] == expected
    assert state[9] == 0xAABBCCDD
    state, *_ = diff(compiled, f'unaligned_reverse_{offset}')
    assert state[8] == expected
    for name in ('swl', 'swr'):
        _, memory, *_ = diff(compiled, f'{name}_{offset}')
        expected_bytes = bytearray(bytes.fromhex('11223344'))
        value = bytes.fromhex('ddccbbaa')
        if name == 'swl': expected_bytes[:offset + 1] = value[3 - offset:]
        else: expected_bytes[offset:] = value[:4 - offset]
        assert memory[DATA & 0x1FFFFF:(DATA & 0x1FFFFF) + 4] == expected_bytes


def test_branch_operands_captured_before_slot(compiled):
    state, *_ = diff(compiled, 'branch_delay', {8: 5, 9: 5})
    assert state[8] == 17 and state[10] == 0
    state, *_ = diff(compiled, 'branch_delay', {8: 4, 9: 5})
    assert state[10] == 22
    state, *_ = diff(compiled, 'load_branch', {8: 5, 9: 5})
    assert state[10] == 0x44332211 and state[11] == 0
    state, *_ = diff(compiled, 'loop', {8: 17, 9: 0})
    assert state[8:10] == [0, 17]


@pytest.mark.parametrize('name', ['call_direct', 'call_indirect'])
def test_call_boundary_load(compiled, name):
    state, _, counts, _ = diff(compiled, name)
    assert state[9:11] == [0xAABBCCDD, 0x44332211]
    assert counts[1] >= 2
    assert counts[0] == 0


def test_dispatch_miss_hands_back_to_compiled(compiled):
    # Runtime-generated trampoline is deliberately absent from the table.
    start = 0x800F0000
    memory = {start: j(2, PCS['callee']), start + 4: imm(9, 0, 8, 99)}
    state, _, counts, _ = diff(compiled, 'callee', memory=memory, start=start)
    assert state[9:11] == [99, 99]
    assert counts[0] == 2 and counts[1] >= 1 and counts[2] == 1
    state, _, counts, _ = diff(compiled, 'fallback', {21: start}, memory)
    assert state[9:11] == [99, 99]
    assert counts[0] == 2 and counts[1] >= 2 and counts[2] == 1


def test_jalr_alternate_link(compiled):
    start = 0x800F0000
    state, *_ = diff(compiled, 'call_alt_link', {21: start}, {start: r(8, rs=22), start + 4: 0})
    assert state[8] == 66 and state[22] == PCS['call_alt_link'] + 8


@pytest.mark.parametrize('name,value', [('link_branch', 4), ('link_branch', -4),
                                        ('link_branch_negative', 4), ('link_branch_negative', -4)])
def test_conditional_link(compiled, name, value):
    state, *_ = diff(compiled, name, {8: value})
    assert state[12] == 77


@pytest.mark.parametrize('index,expected', [(0, 42), (1, 77)])
def test_jump_table(compiled, index, expected):
    assert hex(table_pc + 24) in compiled[2]['jump_tables']
    state, _, counts, _ = diff(compiled, 'jump_table', {4: index})
    assert state[2] == expected and counts[0] == 0


@pytest.mark.parametrize('name,regs,code', [
    ('overflow_add', {8: 0x7FFFFFFF, 9: 1}, 12),
    ('overflow_sub', {8: 0x80000000, 9: 1}, 12),
    ('overflow_addi', {8: 0x7FFFFFFF}, 12),
    ('misaligned_load', {}, 4), ('misaligned_store', {}, 5), ('break', {}, 9),
    ('reserved', {}, 10), ('cop_unusable', {}, 11), ('exception_slot', {}, 4),
])
def test_exception_expected(compiled, name, regs, code):
    state, *_ = diff(compiled, name, regs)
    assert state[37] == 1 and (state[52] >> 2) & 31 == code
    assert state[53] == PCS[name]
    assert bool(state[52] >> 31) == (name == 'exception_slot')


@pytest.mark.parametrize('name', [n for n in CASES if n.startswith('random_')])
def test_random_register_states(compiled, name):
    rng = random.Random(name)
    diff(compiled, name, {reg: rng.getrandbits(32) for reg in range(1, 19)})


def test_decoder_coverage():
    for fn, name in SPECIAL.items(): assert decode(fn, BASE).name == name
    for op, name in PRIMARY.items(): assert decode(op << 26, BASE).name == name
    for op in (16, 18):
        for rs, prefix in ((0, 'mfc'), (2, 'cfc'), (4, 'mtc'), (6, 'ctc')):
            assert decode(cop(op, rs, 2, 3), BASE).name == prefix + str(op - 16)


def test_delay_paths_and_isolation(compiled):
    state, *_ = diff(compiled, 'load_slot_taken')
    assert state[10:12] == [0xAABBCCDD, 0x44332211]
    state, *_ = diff(compiled, 'load_slot_fallthrough')
    assert state[9:11] == [0xAABBCCDD, 0x44332211]
    state, *_ = diff(compiled, 'load_slot_overwrite')
    assert state[8] == 77
    state, *_ = diff(compiled, 'load_slot_twice')
    assert state[9:11] == [0xAABBCCDD, 0x88776655]
    state, *_ = diff(compiled, 'call_return_load')
    assert state[9:11] == [0xAABBCCDD, 0x44332211]
    state, *_ = diff(compiled, 'pending_overflow', {9: 0x7fffffff, 10: 1})
    assert state[8] == 0x44332211 and state[37]
    state, memory, *_ = diff(compiled, 'isolation', {8: 0x10000, 9: 0x12345678})
    assert state[10:12] == [0x44332211, 0x12345678]
    assert memory[0x100000:0x100008] == bytes.fromhex('1122334478563412')
    state, *_ = diff(compiled, 'call_indirect', {21: PCS['incoming_load_twice']})
    assert state[9:11] == [0xAABBCCDD, 0x88776655]
    state, *_ = diff(compiled, 'load_merge_address', {8: DATA}, {DATA: 0x12345678})
    assert state[8] == 0x12345678


@pytest.mark.parametrize('base', [0x00200000, 0x80400000, 0xA0600000, 0x1F800000])
def test_generated_memory_mirrors_and_scratchpad(compiled, base):
    diff(compiled, 'memory', {20: base + 0x100})


@pytest.mark.parametrize('legacy', [False, True])
def test_stop_unwinds_before_following_instruction(compiled, legacy):
    lib, data, _ = compiled
    c = lib.test_new()
    try:
        lib.test_image(c, BASE, data, len(data))
        lib.test_register(c, 8, 0x7fffffff)
        lib.test_register(c, 10, 123)
        if legacy: lib.test_run_legacy(c, PCS['overflow_addi'])
        else: lib.test_run(c, PCS['overflow_addi'], 0)
        snapshot = (C.c_uint32 * 135)()
        lib.test_snapshot(c, snapshot)
        assert snapshot[37] and snapshot[10] == 123
        assert snapshot[34] == PCS['overflow_addi']
        assert lib.test_count(c, 4) == 1
    finally:
        lib.test_free(c)


@pytest.mark.parametrize('interpret', [False, True])
def test_instruction_budget_stops_loop(compiled, interpret):
    lib, data, _ = compiled
    c = lib.test_new()
    try:
        lib.test_image(c, BASE, data, len(data))
        lib.test_register(c, 8, 1000000)
        lib.test_limits(c, 10000, 0)
        lib.test_run(c, PCS['loop'], interpret)
        assert lib.test_reason(c) == b'instruction budget exhausted'
        count = lib.test_count(c, 0) + lib.test_count(c, 4)
        assert 10000 <= count < 10004
    finally:
        lib.test_free(c)


@pytest.mark.parametrize('name', ['callee', 'call_direct'])
def test_modified_source_is_interpreted(compiled, name):
    state, _, counts, _ = diff(compiled, name, memory={PCS['callee']: imm(9, 0, 9, 123)})
    assert state[9] == 123
    assert counts[0] >= 1


@pytest.mark.parametrize('address,width', [(PCS['callee'], 4), (PCS['callee'] + 2, 2),
                                         (PCS['callee'] + 3, 1), (PCS['callee'] + 0x200000, 4)])
def test_cached_source_invalidated_and_restored(compiled, address, width):
    lib, data, _ = compiled
    c = lib.test_new()
    try:
        lib.test_image(c, BASE, data, len(data))
        lib.test_register(c, 8, 99)
        lib.test_run(c, PCS['callee'], 0)
        assert lib.test_count(c, 0) == 0
        original = lib.test_read(c, address, width)
        replacement = imm(9, 0, 9, 123) >> ((address & 3) * 8)
        lib.test_write(c, address, replacement, width)
        lib.test_run(c, PCS['callee'], 0)
        interpreted = lib.test_count(c, 0)
        assert interpreted > 0
        lib.test_write(c, address, original, width)
        lib.test_run(c, PCS['callee'], 0)
        assert lib.test_count(c, 0) == interpreted
    finally:
        lib.test_free(c)


def test_discovery_excludes_buffers_and_outside_code_ranges():
    data = bytearray(512)
    struct.pack_into('<4I', data, 0, j(3, BASE + 128), 0, *RET)
    struct.pack_into('<I', data, 256, j(3, BASE + 384))
    image = Image(bytes(data), BASE, BASE, code_ranges=((BASE, BASE + 128),))
    functions = discover(image)
    assert set(functions) == {BASE}
    assert all(BASE <= pc < BASE + 128 for f in functions.values() for pc in f.instructions)
    assert not discover(Image(bytes(128), BASE, BASE))


def test_store_to_current_function_exits_before_stale_instruction(compiled):
    state, _, counts, _ = diff(compiled, 'self_modify',
                               {20: PCS['self_modify'] + 4, 8: imm(9, 0, 9, 77)})
    assert state[9] == 77
    assert counts[0] > 0


def test_gte_store_to_current_function_exits_before_stale_instruction(compiled):
    state, _, counts, _ = diff(compiled, 'gte_self_modify',
                               {20: PCS['gte_self_modify'] + 8, 8: imm(9, 0, 9, 77)})
    assert state[9] == 77 and counts[0] > 0


def test_dispatch_cache_follows_registry_changes(compiled):
    lib, data, _ = compiled
    c = lib.test_new()
    try:
        lib.test_image(c, BASE, data, len(data))
        lib.test_run(c, PCS['callee'], 0)
        assert lib.test_count(c, 0) == 0
        lib.test_registry_count(c, 0, 0)
        lib.test_run(c, PCS['callee'], 0)
        assert lib.test_count(c, 0) > 0
        before = lib.test_count(c, 0)
        # Read the generated entry count from its exported C symbol.
        count = C.c_size_t.in_dll(lib, 'generated_count').value
        lib.test_registry_count(c, 0, count)
        lib.test_run(c, PCS['callee'], 0)
        assert lib.test_count(c, 0) == before
    finally:
        lib.test_free(c)


def test_overlay_function_selects_replacements_and_rejects_unknown(compiled):
    lib, data, _ = compiled
    c = lib.test_new()
    try:
        lib.test_image(c, BASE, data, len(data))
        for index in (0, 1, 2, 32, 64, 0):
            words = [r(33, 31, 0, 25), j(3, PCS['callee']), 0, imm(9, 0, 2, 42 + index % 32),
                     r(33, 25, 0, 31), *RET]
            blob = struct.pack(f'<{len(words)}I', *words) + bytes(16)
            lib.test_image(c, 0x80098000, blob, len(blob))
            before = lib.test_count(c, 0)
            lib.test_run(c, 0x80098000, 0)
            snapshot = (C.c_uint32 * 135)()
            lib.test_snapshot(c, snapshot)
            assert snapshot[2] == 42 + index % 32
            assert (lib.test_count(c, 0) > before) == (index == 2)
        # Mutable overlay data must not disable an unchanged function.
        lib.test_write(c, 0x80098028, 1, 1)
        before = lib.test_count(c, 0)
        lib.test_run(c, 0x80098000, 0)
        assert lib.test_count(c, 0) == before
    finally:
        lib.test_free(c)


def test_jump_table_words_invalidate_compiled_function(compiled):
    state, _, counts, _ = diff(compiled, 'jump_table', {4: 0}, memory={table: table_pc + 44})
    assert state[2] == 77
    assert counts[0] > 0


def test_overlay_data_write_does_not_change_code_epoch(compiled):
    lib, data, _ = compiled
    c = lib.test_new()
    try:
        lib.test_image(c, BASE, data, len(data))
        words = [r(33, 31, 0, 25), j(3, PCS['callee']), 0, imm(9, 0, 2, 42),
                 r(33, 25, 0, 31), *RET]
        blob = struct.pack(f'<{len(words)}I', *words) + bytes(16)
        lib.test_image(c, 0x80098000, blob, len(blob))
        lib.test_run(c, 0x80098000, 0)
        before = lib.test_code_epoch(c)
        lib.test_write(c, 0x80098028, 1, 1)
        assert lib.test_code_epoch(c) == before
        lib.test_write(c, 0x80098000, 1, 1)
        assert lib.test_code_epoch(c) > before
    finally:
        lib.test_free(c)


@pytest.mark.parametrize('data', [b'', b'abc', b'x' * 55, b'x' * 56, b'x' * 64, bytes(range(256)) * 30])
def test_overlay_sha1_matches_standard(compiled, data):
    import hashlib
    lib, _, _ = compiled
    lib.code_sha1.argtypes = [C.c_void_p, C.c_size_t, C.c_void_p]
    digest = C.create_string_buffer(20)
    lib.code_sha1(data, len(data), digest)
    assert digest.raw == hashlib.sha1(data).digest()


def test_versioned_indirect_entry_points_anchor_discovery():
    data = bytearray(128)
    struct.pack_into('<2I', data, 0, *RET)
    struct.pack_into('<3I', data, 64, imm(9, 0, 2, 77), *RET)
    image = Image(bytes(data), BASE, BASE, entry_points=(BASE + 64,))
    functions = discover(image, scan_calls=False)
    assert set(functions) == {BASE, BASE + 64}
    assert set(functions[BASE + 64].instructions) == {BASE + 64, BASE + 68, BASE + 72}


def test_deep_guest_recursion_resumes_compiled_returns(compiled):
    state, _, counts, _ = diff(compiled, 'deep_recursion', {8: 1000})
    assert state[8] == 0 and state[29] == 0x801ffff0 and not state[37]
    assert counts[0] == 0


@pytest.mark.parametrize('child', ['nonlocal_child', 'unexpected_child'])
def test_nonlocal_reentry_discards_abandoned_call_frames(compiled, child):
    memory = {PCS['nonlocal_loop'] + 4: j(3, PCS[child])} if child == 'unexpected_child' else None
    state, _, counts, _ = diff(compiled, 'nonlocal_loop',
                               {8: 1000, 21: PCS['nonlocal_loop'] + 12}, memory)
    assert state[8] == 0 and not state[37]
    if child == 'nonlocal_child': assert counts[0] == 0


def test_bios_longjmp_resumes_compiled_return_site(compiled):
    saved = [PCS['bios_longjmp'] + 8, 0x801ffff0, 0xffffffff] + [0] * 9
    memory = {DATA + i * 4: value for i, value in enumerate(saved)}
    state, _, counts, _ = diff(compiled, 'bios_longjmp', {4: DATA, 5: 1}, memory)
    assert state[2] == 1 and state[10] == 77 and not state[37]
    assert counts[0] == 0
