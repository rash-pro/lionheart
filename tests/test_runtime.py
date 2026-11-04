import ctypes as C
import struct

import pytest

from test_recompiler import BASE, DATA, Image, compiled, discover, imm, r


@pytest.fixture
def cpu(compiled):
    lib = compiled[0]
    c = lib.test_new()
    assert c
    try:
        yield lib, c
    finally:
        lib.test_free(c)


def snapshot(lib, c):
    out = (C.c_uint32 * 135)()
    lib.test_snapshot(c, out)
    return list(out)


def bios(cpu, table, fn, *args):
    lib, c = cpu
    lib.test_register(c, 9, fn)
    for reg, value in enumerate(args, 4): lib.test_register(c, reg, value)
    lib.test_bios(c, table)
    state = snapshot(lib, c)
    assert not state[37], lib.test_reason(c)
    return state[2]


@pytest.mark.parametrize('mirror', [0, 0x200000, 0x400000, 0x600000, 0x80000000, 0x80200000, 0xA0000000, 0xA0600000])
def test_ram_mirrors(cpu, mirror):
    lib, c = cpu
    lib.test_write(c, 0x104, 0x89ABCDEF, 4)
    assert lib.test_read(c, mirror + 0x104, 4) == 0x89ABCDEF
    lib.test_write(c, mirror + 0x106, 0x1234, 2)
    assert lib.test_read(c, 0x104, 4) == 0x1234CDEF


def test_scratchpad_and_rom(cpu):
    lib, c = cpu
    logs = []
    callback = C.CFUNCTYPE(None, C.c_void_p, C.c_char_p)(lambda _, message: logs.append(message.decode()))
    lib.test_logger(c, callback)
    lib.test_write(c, 0x1F8003FC, 0x12345678, 4)
    assert lib.test_read(c, 0x9F8003FC, 4) == 0x12345678
    assert lib.test_read(c, 0x1FC00100, 4) == 0
    assert lib.test_read(c, 0xBFC00000, 4) == 0
    assert len(logs) == 2 and all('UNMAPPED read' in line for line in logs)


def test_mmio_logging_and_defaults(cpu):
    lib, c = cpu
    logs = []
    callback = C.CFUNCTYPE(None, C.c_void_p, C.c_char_p)(lambda _, message: logs.append(message.decode()))
    lib.test_logger(c, callback)
    lib.test_write(c, 0x1F801074, 0x321, 2)
    assert lib.test_read(c, 0xBF801074, 2) == 0x321
    assert lib.test_read(c, 0x1F801814, 4) & 0x14000000 == 0x14000000  # ready for command and DMA; VRAM→CPU only during a transfer
    assert lib.test_read(c, 0x1F801800, 1) == 0x18
    assert len(logs) == 4
    assert 'MMIO W address=1f801074 width=2 value=00000321' in logs[0]


def test_stall_ignores_unchanged_mmio(cpu):
    lib, c = cpu
    for _ in range(6):
        lib.test_read(c, 0x1F801814, 4)
        lib.test_poll(c, BASE, 5)
    assert 'stall' in lib.test_reason(c).decode()


def test_stall_resets_on_mmio_change(cpu):
    lib, c = cpu
    for i in range(10):
        lib.test_write(c, 0x1F801074, i, 2)
        lib.test_poll(c, BASE, 3)
    assert not snapshot(lib, c)[37]


def test_stall_with_different_read_and_write_values(cpu):
    lib, c = cpu
    for _ in range(10):
        lib.test_write(c, 0x1F801814, 0, 4)
        lib.test_read(c, 0x1F801814, 4)
        lib.test_poll(c, BASE, 5)
    assert b'stall' in lib.test_reason(c)


def test_bios_memory_strings_and_random(cpu):
    lib, c = cpu
    lib.test_write(c, DATA, 0x636261, 4)
    assert bios(cpu, 0, 0x1B, DATA) == 3
    assert bios(cpu, 0, 0x2A, DATA + 16, DATA, 4) == DATA + 16
    assert lib.test_read(c, DATA + 16, 4) == 0x636261
    assert bios(cpu, 0, 0x2B, DATA + 20, 0xAA, 4) == DATA + 20
    assert lib.test_read(c, DATA + 20, 4) == 0xAAAAAAAA
    bios(cpu, 0, 0x2C, DATA + 1, DATA, 3)
    assert lib.test_read(c, DATA, 4) == 0x63626161
    bios(cpu, 0, 0x30, 1)
    assert bios(cpu, 0, 0x2F) == 16838
    assert bios(cpu, 0, 0x2F) == 5758


def test_bios_stubs_and_unknown_call(cpu):
    lib, c = cpu
    one = bios(cpu, 1, 8, 0xF0000001, 2, 0x1000, 0)
    two = bios(cpu, 1, 8, 0xF0000001, 2, 0x1000, 0)
    assert one != two and one & 0xFFFF0000 == 0xF1000000
    assert bios(cpu, 1, 0x0C, one) == 1
    assert bios(cpu, 1, 0x0E, BASE, DATA, 0) & 0xFF000000 == 0xFF000000
    lib.test_register(c, 9, 0x42)
    lib.test_bios(c, 0)
    assert b'unimplemented BIOS A0:42' in lib.test_reason(c)


def test_critical_sections(cpu):
    lib, c = cpu
    lib.test_register(c, 2, 123)
    for number, expected in ((2, 123), (1, 1), (1, 0), (2, 0)):
        lib.test_register(c, 4, number)
        lib.test_syscall(c)
        state = snapshot(lib, c)
        assert bool(state[51] & 0x401) == (number == 2)
        assert state[2] == expected


def test_gte_transfer_registers(cpu):
    lib, c = cpu
    lib.test_gte_write(c, 1, 0x12348001, 0)
    assert lib.test_gte_read(c, 1, 0) == 0xFFFF8001
    lib.test_gte_write(c, 7, 0x12348001, 0)
    assert lib.test_gte_read(c, 7, 0) == 0x8001
    for value in (1, 2, 3): lib.test_gte_write(c, 15, value, 0)
    assert [lib.test_gte_read(c, reg, 0) for reg in (12, 13, 14, 15)] == [1, 2, 3, 3]
    lib.test_gte_write(c, 28, 0x7FFF, 0)
    assert lib.test_gte_read(c, 29, 0) == 0x7FFF
    assert lib.test_gte_read(c, 9, 0) == 0xF80
    lib.test_gte_write(c, 9, 0xFFFF, 0)
    assert lib.test_gte_read(c, 29, 0) == 0x7FE0
    lib.test_gte_write(c, 26, 0x8000, 1)
    assert lib.test_gte_read(c, 26, 1) == 0xFFFF8000
    lib.test_gte_write(c, 31, 0xFFFFFFFF, 1)
    assert lib.test_gte_read(c, 31, 1) == 0xFFFFF000


@pytest.mark.parametrize('value,expected', [(0, 32), (0xFFFFFFFF, 32), (0x80000000, 1), (0x40000000, 1), (0xFFFF0000, 16)])
def test_gte_count_leading_bits(cpu, value, expected):
    lib, c = cpu
    lib.test_gte_write(c, 30, value, 0)
    assert lib.test_gte_read(c, 31, 0) == expected
    lib.test_gte_write(c, 31, 123, 0)
    assert lib.test_gte_read(c, 31, 0) == expected


def test_pending_load_survives_faulting_write(cpu):
    lib, c = cpu
    lib.test_register(c, 20, DATA)
    lib.test_write(c, DATA, 123, 4)
    lib.test_write(c, BASE, imm(35, 20, 8, 0), 4)
    lib.test_write(c, BASE + 4, imm(35, 20, 8, 1), 4)
    lib.test_step(c, BASE)
    lib.test_step(c, BASE + 4)
    state = snapshot(lib, c)
    assert state[8] == 123 and state[37]


def test_discovery_does_not_translate_zero_buffers():
    image = Image(struct.pack('<2I', r(8, rs=31), 0) + bytes(100000), BASE, BASE)
    found = discover(image)
    assert len(found) == 1 and len(found[BASE].instructions) == 2


def test_exe_validation(tmp_path):
    path = tmp_path / 'bad.exe'
    path.write_bytes(b'PS-X EXE')
    with pytest.raises(ValueError): Image.read(path)
    data = bytearray(0x808)
    data[:8] = b'PS-X EXE'
    struct.pack_into('<4I', data, 0x10, BASE, 0, BASE, 16)
    path.write_bytes(data)
    with pytest.raises(ValueError, match='truncated'): Image.read(path)
