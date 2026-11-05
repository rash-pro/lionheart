"""Hand-assembled MIPS programs exercise the actual interpreter and GP0 parser."""
import csv
import ctypes as C
import importlib.util
import os
from pathlib import Path
import subprocess
import struct

import pytest

ROOT = Path(__file__).resolve().parents[1]
PROJECT = 2 << 62
MIXED = (1 << 64) - 1


class Corner(C.Structure):
    _fields_ = [(name, C.c_uint64) for name in ('vertex', 'context', 'generation', 'collection')] + [
        (name, C.c_uint32) for name in ('pc', 'occurrence', 'input_xy', 'input_z')] + [('matrix', C.c_uint32 * 8)]


def ins(op, rt, rs=1, immediate=0):
    return op << 26 | rs << 21 | rt << 16 | (immediate & 65535)


def cop(rs, rt, rd):
    return 18 << 26 | rs << 21 | rt << 16 | rd << 11


@pytest.fixture(scope="module")
def lib(tmp_path_factory):
    path = tmp_path_factory.mktemp("frame-tags") / "tags.so"
    sources = sorted((ROOT / "runtime").glob("*.c"))
    cmd = ["/usr/bin/gcc", "-std=c99", "-O1", "-g", "-shared", "-fPIC", "-Wall", "-Wextra",
           "-Wpedantic", "-Werror", "-DFF8_FRAME_PROVENANCE", "-DFF8_RECORD_FRAME_ORACLE", "-fsanitize=undefined",
           "-fno-sanitize-recover=all", "-Iruntime", "tests/frame_tags_harness.c",
           *map(str, sources), "-o", str(path)]
    subprocess.run(cmd, cwd=ROOT, check=True, capture_output=True)
    lib = C.CDLL(str(path))
    lib.tags_new.restype = C.c_void_p
    for name, types in {
        "free": [], "reset": [], "pause": [C.c_uint], "reg": [C.c_uint, C.c_uint32], "value": [C.c_uint],
        "video": [C.c_uint, C.c_uint],
        "corners": [C.c_uint, C.c_uint, C.POINTER(Corner)],
        "write": [C.c_uint32, C.c_uint32], "read": [C.c_uint32],
        "memory": [C.c_uint32, C.c_uint], "register": [C.c_uint],
        "gte": [C.c_uint, C.c_uint], "step": [C.c_uint32], "halted": [],
        "control": [C.c_uint, C.c_uint32], "gte_value": [C.c_uint],
        "dma": [C.c_uint32], "bios": [C.c_uint], "thread": [C.c_uint, C.c_uint32], "frame": [C.c_uint],
    }.items():
        getattr(lib, "tags_" + name).argtypes = [C.c_void_p, *types]
    for name in ("memory", "register", "gte"):
        getattr(lib, "tags_" + name).restype = C.c_uint64
    lib.tags_read.restype = lib.tags_value.restype = lib.tags_gte_value.restype = C.c_uint32
    return lib


@pytest.fixture
def cpu(lib):
    c = lib.tags_new()
    assert c
    try:
        yield c
        assert not lib.tags_halted(c)
    finally:
        lib.tags_free(c)


def step(lib, cpu, *words):
    for w in words:
        lib.tags_step(cpu, w)


def project(lib, c):
    lib.tags_control(c, 0, 4096)
    lib.tags_control(c, 2, 4096)
    lib.tags_control(c, 4, 4096)
    lib.tags_control(c, 26, 128)
    lib.tags_reg(c, 1, 0x20000)
    for v in range(3):
        # Different input positions quantize to the same screen coordinates.
        lib.tags_write(c, 0x20000 + v * 8, v)
        lib.tags_write(c, 0x20004 + v * 8, 32000)
        step(lib, c, ins(50, v * 2, immediate=v * 8), ins(50, v * 2 + 1, immediate=v * 8 + 4))
    step(lib, c, 0x4A080030)
    return [lib.tags_gte(c, r, 0) for r in (12, 13, 14)]


def test_ram_scratch_copy_chain_and_delayed_load(lib, cpu):
    lib.tags_reg(cpu, 1, 0x20000)
    lib.tags_reg(cpu, 4, 0x1F800080)
    lib.tags_write(cpu, 0x20000, 0x11223344)
    origin = lib.tags_memory(cpu, 0x20000, 4)
    step(lib, cpu, ins(35, 2))
    assert lib.tags_register(cpu, 2) == 0
    step(lib, cpu, 0, ins(43, 2, rs=4), ins(35, 3, rs=4), 0, ins(43, 3, immediate=16))
    assert lib.tags_read(cpu, 0x20010) == 0x11223344
    assert lib.tags_memory(cpu, 0x20010, 4) == origin
    assert lib.tags_register(cpu, 3) == origin


def test_same_value_overwrite_and_reused_buffer_get_new_generation(lib, cpu):
    lib.tags_write(cpu, 0x20000, 17)
    before = lib.tags_memory(cpu, 0x20000, 4)
    lib.tags_write(cpu, 0x20000, 17)
    assert lib.tags_memory(cpu, 0x20000, 4) != before
    lib.tags_reg(cpu, 1, 0x20000)
    lib.tags_reg(cpu, 2, 17)
    before = lib.tags_memory(cpu, 0x20000, 4)
    step(lib, cpu, ins(43, 2))
    assert lib.tags_memory(cpu, 0x20000, 4) != before


def test_equal_rtpt_outputs_keep_fifo_origins(lib, cpu):
    tags = project(lib, cpu)
    assert len(set(tags)) == 3 and all(t & PROJECT for t in tags)
    assert len({lib.tags_gte_value(cpu, r) for r in (12, 13, 14)}) == 1
    assert lib.tags_gte(cpu, 15, 0) == tags[2]
    assert [lib.tags_gte(cpu, r, 0) for r in (17, 18, 19)] == tags
    step(lib, cpu, cop(0, 2, 12), 0, ins(43, 2, immediate=32), ins(58, 13, immediate=36))
    assert lib.tags_memory(cpu, 0x20020, 4) == tags[0]
    assert lib.tags_memory(cpu, 0x20024, 4) == tags[1]


def test_live_corners_keep_shared_quad_vertices_and_actual_inputs(lib, cpu):
    project(lib, cpu)
    first, second = (Corner * 3)(), (Corner * 3)()
    assert lib.tags_corners(cpu, 0, 0, first)
    assert lib.tags_corners(cpu, 1, 0, second)
    assert [c.input_xy for c in first] == [0, 1, 2]
    assert [c.input_z for c in first] == [32000] * 3
    assert bytes(first[1]) == bytes(second[0])
    assert bytes(first[2]) == bytes(second[1])
    assert bytes(first[0]) == bytes(second[2])
    assert list(first[0].matrix) == [4096, 0, 4096, 0, 4096, 0, 0, 0]
    assert not lib.tags_corners(cpu, 0, 2, first)
    assert not first[1].vertex


def test_restore_discards_register_and_matrix_provenance(lib, cpu):
    project(lib, cpu)
    value = lib.tags_gte_value(cpu, 14)
    lib.tags_reset(cpu)
    assert lib.tags_gte_value(cpu, 14) == value
    step(lib, cpu, 0x4A080030)
    assert lib.tags_gte(cpu, 14, 0) == (1 << 64) - 2
    assert lib.tags_register(cpu, 0) == 0


def test_movie_pause_keeps_guest_results_and_resumes_unknown(lib, cpu):
    project(lib, cpu)
    value = lib.tags_gte_value(cpu, 14)
    lib.tags_pause(cpu, 1)
    step(lib, cpu, 0x4A080030)
    assert lib.tags_gte_value(cpu, 14) == value
    assert lib.tags_gte(cpu, 14, 0) == 0
    lib.tags_pause(cpu, 0)
    step(lib, cpu, 0x4A080030)
    assert lib.tags_gte_value(cpu, 14) == value
    assert lib.tags_gte(cpu, 14, 0) == (1 << 64) - 2


def test_rgb15_decoder_gaps_remain_excluded(lib, cpu):
    project(lib, cpu)
    lib.tags_video(cpu, 100, 10)
    assert lib.tags_gte(cpu, 14, 0) == 0
    lib.tags_video(cpu, 107, 10)
    assert lib.tags_gte(cpu, 14, 0) == 0
    lib.tags_video(cpu, 108, 10)
    assert lib.tags_gte(cpu, 14, 0) == (1 << 64) - 2


def test_coordinate_arithmetic_preserves_single_origin_and_rejects_mix(lib, cpu):
    tags = project(lib, cpu)
    step(lib, cpu, cop(0, 2, 12), 0, ins(9, 2, rs=2, immediate=4))
    assert lib.tags_register(cpu, 2) == tags[0]
    step(lib, cpu, cop(0, 3, 13), 0, (2 << 21) | (3 << 16) | (4 << 11) | 33)
    assert lib.tags_register(cpu, 4) == MIXED
    step(lib, cpu, (2 << 21) | 17, (5 << 11) | 16)
    assert lib.tags_register(cpu, 5) == tags[0]


def test_partial_stores_and_load_cancel(lib, cpu):
    tags = project(lib, cpu)
    step(lib, cpu, cop(0, 2, 12), 0, ins(41, 2, immediate=40), ins(41, 2, immediate=42))
    assert lib.tags_memory(cpu, 0x20028, 4) == tags[0]
    step(lib, cpu, ins(35, 3, immediate=40), ins(9, 3, rs=0, immediate=1), 0)
    assert lib.tags_register(cpu, 3) == 0
    assert lib.tags_value(cpu, 3) == 1


def test_bios_copy_and_thread_restore(lib, cpu):
    tags = project(lib, cpu)
    step(lib, cpu, ins(58, 12, immediate=32))
    for reg, value in ((4, 0x30000), (5, 0x20020), (6, 4), (9, 0x2A)):
        lib.tags_reg(cpu, reg, value)
    lib.tags_bios(cpu, 0)
    assert lib.tags_memory(cpu, 0x30000, 4) == tags[0]
    step(lib, cpu, cop(0, 2, 12), 0)
    lib.tags_thread(cpu, 0, 0x40000)
    lib.tags_reg(cpu, 2, 0)
    lib.tags_thread(cpu, 1, 0x40000)
    assert lib.tags_register(cpu, 2) == tags[0]


def test_dma_linked_list_and_gp0_port_use_exact_corner_tags(lib, tmp_path, monkeypatch):
    trace = tmp_path / "tags.csv"
    monkeypatch.setenv("FF8_PROVENANCE_TRACE", str(trace))
    c = lib.tags_new()
    try:
        tags = project(lib, c)
        lib.tags_reg(c, 1, 0x30000)
        lib.tags_write(c, 0x30000, 0x04FFFFFF)
        lib.tags_write(c, 0x30004, 0x200000FF)
        for i, reg in enumerate((12, 13, 14)):
            step(lib, c, ins(58, reg, immediate=8 + i * 4))
        lib.tags_dma(c, 0x30000)
        lib.tags_reg(c, 1, 0x1F801810)
        lib.tags_reg(c, 2, 0x200000FF)
        step(lib, c, ins(43, 2), *(ins(58, r) for r in (12, 13, 14)))
        assert not lib.tags_halted(c)
    finally:
        lib.tags_free(c)
    rows = list(csv.DictReader(trace.open()))
    corners = [r for r in rows if r["kind"].startswith("C")]
    assert [r["kind"] for r in corners] == ["C1"] * 6
    assert [int(r["tag"]) for r in corners] == tags * 2


def test_independent_oracle_captures_raw_inputs_and_rejects_corrupted_identity(lib, tmp_path, monkeypatch):
    trace, oracle = tmp_path / "tags.csv", tmp_path / "oracle.bin"
    monkeypatch.setenv("FF8_PROVENANCE_TRACE", str(trace))
    monkeypatch.setenv("FF8_FRAME_ORACLE", str(oracle))
    c = lib.tags_new()
    try:
        project(lib, c)
        for frame in range(2):
            lib.tags_frame(c, 320 if frame == 0 else 0)
            step(lib, c, 0x4A080030)
            lib.tags_reg(c, 1, 0x1F801810)
            lib.tags_reg(c, 2, 0x200000FF)
            step(lib, c, ins(43, 2), *(ins(58, r) for r in (12, 13, 14)))
    finally:
        lib.tags_free(c)
    record = struct.Struct("<QIIII8I8I8Q")
    data = oracle.read_bytes()
    assert data[:16] == b"FRAMEORACLE00001"
    raw = [record.unpack_from(data, at) for at in range(16, len(data), record.size)]
    assert [r[3] for r in raw] == [0, 1, 2] * 3
    assert all(r[4] == 32000 for r in raw)
    assert all(r[5:13] == (4096, 0, 4096, 0, 4096, 0, 0, 0) for r in raw)
    spec = importlib.util.spec_from_file_location("correspondence", ROOT / "tools/verify_frame_correspondence.py")
    audit = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(audit)
    # Primary tag rows are deliberately edited; the independent binary is left
    # unchanged. Swapping two identities with identical SXY must be rejected.
    rows = list(csv.DictReader(trace.open()))
    last = [r for r in rows if r["kind"] == "C1" and r["frame"] == "2"]
    assert len(last) == 3
    last[0]["vertex"], last[1]["vertex"] = last[1]["vertex"], last[0]["vertex"]
    with trace.open("w") as stream:
        writer = csv.DictWriter(stream, fieldnames=rows[0].keys())
        writer.writeheader(); writer.writerows(rows)
    result = audit.inspect(trace, oracle, 0, 10)
    assert result["counts"]["input_xyz_mismatch"] == 2


def test_halfword_sources_and_vector_roles_do_not_alias(lib, tmp_path, monkeypatch):
    trace = tmp_path / "tags.csv"
    monkeypatch.setenv("FF8_PROVENANCE_TRACE", str(trace))
    c = lib.tags_new()
    try:
        project(lib, c)
        lib.tags_reg(c, 1, 0x20000)
        lib.tags_write(c, 0x20000, 0x10201020)
        # Equal values from separate packed halfwords retain distinct sources.
        step(lib, c, ins(37, 2), 0, cop(4, 2, 0), cop(4, 2, 1), 0x4A080001)
        step(lib, c, ins(37, 2, immediate=2), 0, cop(4, 2, 0), cop(4, 2, 1), 0x4A080001)
        # Equal sets of dependencies do not erase the XY/Z input roles.
        step(lib, c, ins(35, 2), 0, cop(4, 2, 0), cop(4, 2, 1), 0x4A080001)
        step(lib, c, cop(4, 0, 1), 0x4A080001)
    finally:
        lib.tags_free(c)
    vertices = [r["vertex"] for r in csv.DictReader(trace.open()) if r["kind"] == "P"][-4:]
    assert len(set(vertices)) == 4


def test_control_moves_and_bulk_reload_lifetimes(lib, tmp_path, monkeypatch):
    trace = tmp_path / "tags.csv"
    monkeypatch.setenv("FF8_PROVENANCE_TRACE", str(trace))
    c = lib.tags_new()
    try:
        project(lib, c)
        lib.tags_reg(c, 1, 0x30000)
        lib.tags_write(c, 0x30000, 4096)
        step(lib, c, ins(35, 2), 0, cop(6, 2, 0), 0x4A080001)
        step(lib, c, cop(2, 3, 0), 0, cop(6, 3, 0), 0x4A080001)
        # A CPU bulk fill starts a new matrix-buffer lifetime even for equal bits.
        for reg, value in ((4, 0x30000), (5, 0x30004), (6, 4), (9, 0x2A)):
            lib.tags_reg(c, reg, value)
        lib.tags_write(c, 0x30004, 4096)
        lib.tags_bios(c, 0)
        step(lib, c, ins(35, 2), 0, cop(6, 2, 0), 0x4A080001)
    finally:
        lib.tags_free(c)
    contexts = [r["context"] for r in csv.DictReader(trace.open()) if r["kind"] == "P"][-3:]
    assert contexts[0] == contexts[1]
    assert contexts[2] != contexts[1]
