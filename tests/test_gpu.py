"""Hand-derived GP0/GP1 cases; no disc, generated code, or emulator reference images.

Rules: https://psx-spx.consoledev.net/graphicsprocessingunitgpu/
Triangle samples are integer positions with top/left inclusive, bottom/right exclusive.
"""
import ctypes as C
import itertools
import os
from pathlib import Path
import shlex
import struct
import subprocess
import zlib

import pytest

ROOT = Path(__file__).resolve().parents[1]


@pytest.fixture(scope="session")
def gpu_lib(tmp_path_factory):
    path = tmp_path_factory.mktemp("gpu") / "gpu.so"
    flags = shlex.split(subprocess.check_output(["pkg-config", "--cflags", "--libs", "sdl2"], text=True))
    sanitize = ["-fsanitize=undefined", "-fno-sanitize-recover=all"] if os.environ.get("FF8_TEST_SANITIZE") else []
    subprocess.run(["cc", "-shared", "-fPIC", "-std=c99", "-O2", "-Wall", "-Wextra", "-Wpedantic", "-Werror",
                    *sanitize, "-Iruntime", "-Ibackends/host", "runtime/gpu.c", "runtime/gpu_raster_soft.c",
                    "backends/host/display.c", "backends/host/screenshot.c", "backends/host/osd.c", "backends/host/viewport.c", "tests/gpu_harness.c", "-o", str(path), *flags, "-lz"], cwd=ROOT, check=True)
    lib = C.CDLL(str(path))
    lib.gpu_create.restype = C.c_void_p
    for name in ("gpu_gp0", "gpu_gp1"):
        getattr(lib, name).argtypes = [C.c_void_p, C.c_uint32]
    for name in ("gpu_read", "gpu_status"):
        getattr(lib, name).argtypes = [C.c_void_p]
        getattr(lib, name).restype = C.c_uint32
    lib.gpu_vblank.argtypes = lib.test_gpu_free.argtypes = [C.c_void_p]
    lib.gpu_gp0_words.argtypes = [C.c_void_p, C.POINTER(C.c_uint8), C.c_uint]
    lib.test_gpu_feed.argtypes = [C.c_void_p, C.POINTER(C.c_uint32), C.c_size_t]
    lib.test_gpu_pixel.argtypes = [C.c_void_p, C.c_uint, C.c_uint]
    lib.test_gpu_pixel.restype = C.c_uint16
    lib.gpu_display.argtypes = [C.c_void_p] + [C.POINTER(C.c_int)] * 6
    lib.host_display_rgb.argtypes = [C.c_void_p, C.POINTER(C.c_uint8), C.c_size_t, C.POINTER(C.c_int), C.POINTER(C.c_int)]
    lib.host_display_dump.argtypes = lib.host_vram_dump.argtypes = [C.c_void_p, C.c_char_p]
    lib.host_display_init.argtypes = [C.c_int, C.c_char_p, C.c_char_p]
    lib.host_present.argtypes = lib.host_display_shutdown.argtypes = [C.c_void_p]
    return lib


class GPU:
    def __init__(self, lib):
        self.lib, self.g = lib, lib.gpu_create()
        assert self.g

    def gp0(self, *words):
        self.lib.test_gpu_feed(self.g, (C.c_uint32 * len(words))(*words), len(words))

    def gp1(self, word):
        self.lib.gpu_gp1(self.g, word)

    def pixel(self, x, y):
        return self.lib.test_gpu_pixel(self.g, x, y)

    def image(self, x, y, w, h):
        return [[self.pixel(x + i, y + j) for i in range(w)] for j in range(h)]

    def upload(self, x, y, w, h, pixels):
        assert len(pixels) == (w or 1024) * (h or 512)
        pixels = list(pixels)
        if len(pixels) & 1:
            pixels.append(0xDEAD)
        self.gp0(0xA0000000, xy(x, y), xy(w, h),
                 *(pixels[i] | (pixels[i + 1] << 16) for i in range(0, len(pixels), 2)))

    def display(self, w=4, h=2, depth=False, x=0, y=0):
        self.gp1(0x03000000)
        self.gp1(0x05000000 | x | (y << 10))
        self.gp1(0x06000000 | 512 | ((512 + w * 10) << 12))
        self.gp1(0x07000000 | 16 | ((16 + h) << 10))
        self.gp1(0x08000000 | (16 if depth else 0))

    def rgb(self):
        out = (C.c_uint8 * (1024 * 512 * 3))()
        w, h = C.c_int(), C.c_int()
        assert self.lib.host_display_rgb(self.g, out, len(out), C.byref(w), C.byref(h))
        return w.value, h.value, bytes(out[:w.value * h.value * 3])

    def status(self):
        return self.lib.gpu_status(self.g)


@pytest.fixture
def gpu(gpu_lib):
    g = GPU(gpu_lib)
    g.gp0(0xE3000000, 0xE4000000 | 1023 | (511 << 10))
    try:
        yield g
    finally:
        gpu_lib.test_gpu_free(g.g)


def xy(x, y):
    return (x & 0xFFFF) | ((y & 0xFFFF) << 16)


def rgb5(r, g=0, b=0):
    return r | (g << 5) | (b << 10)


@pytest.mark.parametrize("order", list(itertools.permutations(range(3))))
def test_triangle_top_left_all_windings(gpu, order):
    vertices = [xy(1, 1), xy(5, 1), xy(1, 5)]
    gpu.gp0(0x200000FF, *(vertices[i] for i in order))
    # x+y < 6, x/y >= 1: ten pixels; the descending diagonal is the right edge.
    assert gpu.image(0, 0, 6, 6) == [
        [0, 0, 0, 0, 0, 0],
        [0, 31, 31, 31, 31, 0],
        [0, 31, 31, 31, 0, 0],
        [0, 31, 31, 0, 0, 0],
        [0, 31, 0, 0, 0, 0],
        [0, 0, 0, 0, 0, 0],
    ]


def test_quad_shared_edge_blends_once(gpu):
    gpu.gp0(0xE1000020)  # additive
    gpu.gp0(0x2A000040, xy(1, 1), xy(5, 1), xy(1, 5), xy(5, 5))
    assert gpu.image(1, 1, 5, 5) == [[8, 8, 8, 8, 0]] * 4 + [[0] * 5]


def test_gouraud_affine_samples_and_packet_fifo(gpu):
    gpu.gp0(0x300000F8, xy(0, 0), 0x0000F800, xy(4, 0), 0x00F80000)
    assert gpu.pixel(0, 0) == 0  # incomplete packet cannot render
    assert not gpu.status() & (1 << 26)
    gpu.gp0(xy(0, 4))
    assert gpu.pixel(0, 0) == rgb5(31)
    assert gpu.pixel(2, 0) == rgb5(15, 15)
    assert gpu.pixel(1, 1) == rgb5(15, 7, 7)
    assert gpu.status() & (1 << 26)


def test_drawing_clip_offset_and_size_rejection(gpu):
    gpu.gp0(0xE3000000 | 3 | (3 << 10), 0xE4000000 | 4 | (4 << 10))
    gpu.gp0(0xE5000000 | 5 | (5 << 11))
    gpu.gp0(0x6000FF00, xy(-3, -3), xy(5, 5))
    assert gpu.image(2, 2, 4, 4) == [[0]*4, [0, 992, 992, 0], [0, 992, 992, 0], [0]*4]
    gpu.gp0(0x200000FF, xy(-600, 0), xy(600, 0), xy(0, 100))
    assert gpu.pixel(3, 3) == 992


@pytest.mark.parametrize("size,op", [(1, 0x68), (8, 0x70), (16, 0x78)])
def test_fixed_rectangles(gpu, size, op):
    gpu.gp0((op << 24) | 0xFF0000, xy(7, 8))
    assert gpu.image(7, 8, size, size) == [[0x7C00] * size] * size
    assert gpu.pixel(7 + size, 8) == 0
    assert gpu.pixel(7, 8 + size) == 0


def test_clut4_transparent_and_black_texels(gpu):
    gpu.upload(640, 256, 1, 1, [0x3210])
    gpu.upload(512, 300, 4, 1, [0, 31, 992, 0x8000])
    gpu.upload(0, 0, 4, 1, [0x7C00] * 4)
    gpu.gp0(0xE100001A)  # x=640, y=256, 4-bit
    gpu.gp0(0x65000000, xy(0, 0), ((300 * 64 + 32) << 16), xy(4, 1))
    assert gpu.image(0, 0, 4, 1) == [[0x7C00, 31, 992, 0x8000]]


def test_clut8_wrap_and_texture_window(gpu):
    gpu.upload(640, 256, 1, 1, [0x0100])
    gpu.upload(1008, 300, 1, 1, [31])
    gpu.upload(1009, 300, 1, 1, [992])
    gpu.gp0(0xE100009A)
    # Mask bit 3 away, sampling u=8 and 9 as u=0 and 1.
    gpu.gp0(0xE2000001)
    gpu.gp0(0x65000000, xy(0, 0), ((300 * 64 + 63) << 16) | 8, xy(2, 1))
    assert gpu.image(0, 0, 2, 1) == [[31, 992]]
    gpu.upload(640, 256, 1, 1, [16])
    gpu.upload(0, 300, 1, 1, [0x7C00])
    gpu.gp0(0x65000000, xy(3, 0), (300 * 64 + 63) << 16, xy(1, 1))
    assert gpu.pixel(3, 0) == 0x7C00


def test_texture_window_offset_and_flip(gpu):
    gpu.upload(648, 256, 2, 1, [31, 992])
    gpu.gp0(0xE100011A, 0xE2000001 | (1 << 10))
    gpu.gp0(0x65000000, xy(0, 0), 0, xy(2, 1))
    assert gpu.image(0, 0, 2, 1) == [[31, 992]]
    gpu.gp0(0xE100111A)
    gpu.gp0(0x65000000, xy(2, 0), 1, xy(2, 1))
    assert gpu.image(2, 0, 2, 1) == [[992, 31]]


@pytest.mark.parametrize("gouraud", [False, True])
def test_textured_quad_packet_tpage_and_modulation(gpu, gouraud):
    gpu.upload(640, 256, 2, 2, [31, 992, 0x7C00, 0x7FFF])
    mode = 0x11A
    gpu.gp0(0xE1000200)  # tpage in polygon must preserve dither
    words = [0x3C808080 if gouraud else 0x2C808080, xy(0, 0), 0]
    for coord, uv in [(xy(2, 0), (mode << 16) | 2), (xy(0, 2), 2 << 8), (xy(2, 2), (2 << 8) | 2)]:
        if gouraud:
            words.append(0x808080)
        words += [coord, uv]
    gpu.gp0(*words)
    assert gpu.status() & 0x3FF == mode | 512
    # Dither subtracts 4/3 from full-scale channels at x=0, hence 30 there.
    assert gpu.image(0, 0, 2, 2) == [[30, 992], [0x7C00, rgb5(30, 30, 30)]]


@pytest.mark.parametrize("mode,expected", [(0, (10, 21, 15)), (1, (20, 31, 31)), (2, (0, 18, 0)), (3, (10, 31, 7))])
def test_semitransparency_modes(gpu, mode, expected):
    gpu.upload(0, 0, 1, 1, [rgb5(7, 30, 0)])
    gpu.gp0(0xE1000000 | (mode << 5))
    gpu.gp0(0x6A000000 | (13 * 8) | (12 * 8 << 8) | (31 * 8 << 16), xy(0, 0))
    assert gpu.pixel(0, 0) == rgb5(*expected)


def test_texture_semitransparency_bit_and_raw_color(gpu):
    gpu.upload(640, 256, 2, 1, [0x8008, 8])
    gpu.upload(0, 0, 2, 1, [8, 8])
    gpu.gp0(0xE100013A, 0x67000000, xy(0, 0), 0, xy(2, 1))
    assert gpu.image(0, 0, 2, 1) == [[0x8010, 8]]


def test_modulation_neutral_double_and_half(gpu):
    gpu.upload(640, 256, 1, 1, [rgb5(16, 8, 4)])
    gpu.gp0(0xE100011A)
    for x, color in enumerate([0x808080, 0xFFFFFF, 0x404040]):
        gpu.gp0(0x64000000 | color, xy(x, 0), 0, xy(1, 1))
    assert gpu.image(0, 0, 3, 1) == [[rgb5(16, 8, 4), rgb5(31, 15, 7), rgb5(8, 4, 2)]]


def test_mask_set_check_on_draw_upload_and_copy(gpu):
    gpu.gp0(0xE6000001, 0x680000FF, xy(0, 0))
    assert gpu.pixel(0, 0) == 0x801F
    gpu.gp0(0xE6000002, 0x6800FF00, xy(0, 0))
    gpu.upload(0, 0, 2, 1, [0x7C00, 992])
    assert gpu.image(0, 0, 2, 1) == [[0x801F, 992]]
    gpu.gp0(0x80000000, xy(1, 0), xy(0, 0), xy(1, 1))
    assert gpu.pixel(0, 0) == 0x801F
    gpu.gp0(0xE6000001, 0x80000000, xy(1, 0), xy(2, 0), xy(1, 1))
    assert gpu.pixel(2, 0) == 0x83E0


def test_fill_alignment_mask_bypass_and_zero_sizes(gpu):
    gpu.gp0(0xE6000003)
    gpu.upload(16, 0, 1, 1, [0xFFFF])
    gpu.gp0(0x020000FF, xy(19, 0), xy(1, 1))
    assert gpu.image(15, 0, 18, 1) == [[0] + [31] * 16 + [0]]
    gpu.gp0(0x0200FF00, xy(16, 0), xy(1024, 1), 0x0200FF00, xy(16, 0), xy(16, 512))
    assert gpu.pixel(16, 0) == 31


def test_transfer_wrap_odd_padding_and_copy(gpu):
    gpu.upload(1023, 511, 3, 2, [1, 2, 3, 4, 5, 6])
    assert [gpu.pixel(x, y) for y in [511, 0] for x in [1023, 0, 1]] == [1, 2, 3, 4, 5, 6]
    gpu.gp0(0x80000000, xy(1023, 511), xy(20, 20), xy(3, 2))
    assert gpu.image(20, 20, 3, 2) == [[1, 2, 3], [4, 5, 6]]
    gpu.upload(30, 30, 3, 1, [7, 8, 9])
    assert gpu.pixel(33, 30) == gpu.pixel(30, 31) == 0
    gpu.gp0(0xC0000000, xy(30, 30), xy(3, 1))
    gpu.gp1(0x04000003)
    assert gpu.status() & (1 << 27) and gpu.status() & (1 << 25)
    assert gpu.lib.gpu_read(gpu.g) == 0x00080007
    assert gpu.lib.gpu_read(gpu.g) & 0xFFFF == 9
    assert not gpu.status() & (1 << 27) and not gpu.status() & (1 << 25)
    assert gpu.status() & (1 << 26)


def test_zero_transfer_width_means_1024(gpu):
    gpu.upload(0, 0, 0, 1, [31] * 1024)
    assert gpu.image(0, 0, 1024, 1) == [[31] * 1024]
    assert gpu.pixel(0, 1) == 0


def test_overlapping_copy_preserves_small_source_row(gpu):
    gpu.upload(0, 0, 5, 1, [1, 2, 3, 4, 5])
    gpu.gp0(0x80000000, xy(0, 0), xy(1, 0), xy(4, 1))
    assert gpu.image(0, 0, 5, 1) == [[1, 1, 2, 3, 4]]


def test_lines_polyline_terminators_and_joints(gpu):
    gpu.gp0(0xE1000020, 0x4A000040, xy(1, 1), xy(3, 1), xy(3, 3), 0x50005000)
    assert gpu.image(1, 1, 3, 3) == [[8, 8, 8], [0, 0, 8], [0, 0, 8]]
    gpu.gp0(0x580000F8, xy(0, 5), 0x0000F800, xy(2, 5), 0x00F80000, xy(4, 5), 0x5ABC5DEF)
    assert gpu.image(0, 5, 5, 1) == [[31, rgb5(15, 15), 992, rgb5(0, 15, 15), 0x7C00]]
    gpu.gp0(0x400000FF, xy(6, 6), xy(6, 6))
    assert gpu.pixel(6, 6) == 31
    assert gpu.status() & (1 << 26)


def test_dither_matrix_and_primitive_rules(gpu):
    gpu.gp0(0xE1000200)
    gpu.gp0(0x38808080, xy(0, 0), 0x808080, xy(4, 0), 0x808080, xy(0, 4), 0x808080, xy(4, 4))
    expected = [[15, 16, 15, 16], [16, 15, 16, 15], [15, 16, 15, 16], [16, 15, 16, 15]]
    assert gpu.image(0, 0, 4, 4) == [[rgb5(n, n, n) for n in row] for row in expected]
    gpu.gp0(0x60808080, xy(4, 0), xy(4, 4))
    assert gpu.image(4, 0, 4, 4) == [[rgb5(16, 16, 16)] * 4] * 4
    gpu.gp0(0x20808080, xy(8, 0), xy(12, 0), xy(8, 4))
    assert gpu.pixel(8, 0) == rgb5(16, 16, 16)
    gpu.gp0(0x40808080, xy(0, 8), xy(3, 8))
    assert gpu.image(0, 8, 4, 1) == [[rgb5(n, n, n) for n in [15, 16, 15, 16]]]


def test_reset_info_latch_irq_and_dma_polling(gpu):
    gpu.gp1(0)
    assert gpu.status() == 0x14802000
    gpu.gp0(0xE2123456)
    gpu.gp1(0x10000002)
    assert gpu.lib.gpu_read(gpu.g) == 0x23456
    gpu.gp0(0xE2000000)
    assert gpu.lib.gpu_read(gpu.g) == 0x23456
    gpu.gp1(0x11000007)
    assert gpu.lib.gpu_read(gpu.g) == 2
    gpu.gp0(0x1F000000)
    assert gpu.status() & (1 << 24)
    gpu.gp1(0x02000000)
    assert not gpu.status() & (1 << 24)
    for dma in range(4):
        gpu.gp1(0x04000000 | dma)
        assert (gpu.status() >> 29) & 3 == dma
        assert bool(gpu.status() & (1 << 25)) == (dma in (1, 2))
    gpu.gp0(0xA0000000, xy(0, 0), xy(4, 4), 0x12345678)
    assert not gpu.status() & (1 << 26)
    gpu.gp1(0x01000000)
    assert gpu.status() & (1 << 26)
    assert gpu.pixel(0, 0) == 0x5678
    gpu.gp1(0)
    assert gpu.pixel(0, 0) == 0x5678  # Reset does not erase VRAM.


@pytest.mark.parametrize("mode,width", [(0, 256), (1, 320), (2, 512), (3, 640), (64, 368)])
def test_display_ranges_resolution_and_interlace(gpu, mode, width):
    divisor = {0: 10, 1: 8, 2: 5, 3: 4, 64: 7}[mode]
    gpu.gp1(0x06000000 | 512 | ((512 + width * divisor) << 12))
    gpu.gp1(0x07000000 | 16 | ((16 + 240) << 10))
    gpu.gp1(0x08000000 | mode | 36)
    values = [C.c_int() for _ in range(6)]
    gpu.lib.gpu_display(gpu.g, *(C.byref(v) for v in values))
    assert [v.value for v in values[2:4]] == [width, 480]
    before = gpu.status()
    gpu.lib.gpu_vblank(gpu.g)
    assert (before ^ gpu.status()) & ((1 << 31) | (1 << 13)) == (1 << 31) | (1 << 13)


def test_display_24bit_byte_order_origin_wrap_and_disable(gpu):
    row = bytes(range(12))
    gpu.upload(1023, 511, 6, 1, struct.unpack("<6H", row))
    gpu.display(4, 1, True, 1023, 511)
    assert gpu.rgb() == (4, 1, row)
    gpu.gp1(0x03000001)
    assert gpu.rgb() == (4, 1, bytes(12))


def test_display_15bit_conversion_and_wrapped_rows(gpu):
    gpu.upload(1023, 511, 4, 2, [31, 992, 0x7C00, 0xFFFF, 0, 0, 0, 0])
    gpu.display(4, 2, False, 1023, 511)
    assert gpu.rgb() == (4, 2, bytes([255, 0, 0, 0, 255, 0, 0, 0, 255, 255, 255, 255]) + bytes(12))


def read_png(path):
    data = path.read_bytes()
    assert data[:8] == b"\x89PNG\r\n\x1a\n"
    at, compressed, dimensions = 8, b"", None
    while at < len(data):
        n = struct.unpack_from(">I", data, at)[0]
        kind = data[at+4:at+8]
        body = data[at+8:at+8+n]
        assert zlib.crc32(kind + body) == struct.unpack_from(">I", data, at+8+n)[0]
        if kind == b"IHDR":
            w, h, depth, color, comp, filt, interlace = struct.unpack(">IIBBBBB", body)
            assert (depth, color, comp, filt, interlace) == (8, 2, 0, 0, 0)
            dimensions = w, h
        if kind == b"IDAT":
            compressed += body
        at += n + 12
        if kind == b"IEND":
            assert at == len(data)
            break
    raw = zlib.decompress(compressed)
    w, h = dimensions
    stride = w * 3 + 1
    assert len(raw) == stride * h
    assert all(raw[i * stride] == 0 for i in range(h))
    return w, h, b"".join(raw[i * stride+1:(i+1) * stride] for i in range(h))


@pytest.mark.parametrize("headless", [True, False])
def test_present_pngs_and_vram_exit(gpu, tmp_path, monkeypatch, headless):
    monkeypatch.setenv("SDL_VIDEODRIVER", "dummy")
    frames, vram = tmp_path / "nested/frames", tmp_path / "vram.png"
    gpu.display(4, 2)
    gpu.upload(0, 0, 4, 2, [31] * 8)
    assert gpu.lib.host_display_init(int(headless), os.fsencode(frames), os.fsencode(vram))
    try:
        for _ in range(2):
            gpu.lib.gpu_vblank(gpu.g)
            gpu.lib.host_present(gpu.g)
        assert gpu.lib.host_display_running()
    finally:
        assert gpu.lib.host_display_shutdown(gpu.g)
    assert sorted(p.name for p in frames.iterdir()) == ["frame-000000.png", "frame-000001.png"]
    assert read_png(frames / "frame-000000.png") == (4, 2, bytes([255, 0, 0]) * 8)
    w, h, pixels = read_png(vram)
    assert (w, h) == (1024, 512)
    assert pixels[:12] == bytes([255, 0, 0]) * 4


def test_png_output_failure_is_reported(gpu, tmp_path):
    assert gpu.lib.host_display_init(1, None, os.fsencode(tmp_path / "missing/vram.png"))
    assert not gpu.lib.host_display_shutdown(gpu.g)


def test_truncated_packets_and_random_control_streams(gpu):
    # Exercise bounded packet/polyline/transfer state, including reset mid-command.
    import random
    rng = random.Random(0x505358)
    for _ in range(1000):
        gpu.gp0(rng.getrandbits(32), rng.getrandbits(32))
        gpu.gp1(0x01000000)
        gpu.gp1(0x10000000 | rng.randrange(16))
        gpu.lib.gpu_read(gpu.g)
    assert gpu.status() & (1 << 26)


def test_copy_large_horizontal_overlap_and_vertical_feedback(gpu):
    source = list(range(1, 301))
    gpu.upload(0, 0, 300, 1, source)
    gpu.gp0(0x80000000, xy(0, 0), xy(1, 0), xy(299, 1))
    assert gpu.image(0, 0, 300, 1) == [[1] + source[:-1]]
    gpu.upload(0, 10, 2, 3, [1, 2, 3, 4, 5, 6])
    gpu.gp0(0x80000000, xy(0, 10), xy(0, 11), xy(2, 2))
    assert gpu.image(0, 10, 2, 3) == [[1, 2], [1, 2], [1, 2]]


def test_negative_line_coordinates_clip_without_truncation_artifact(gpu):
    gpu.gp0(0x400000FF, xy(-2, -1), xy(2, 1))
    # The first two samples are offscreen, and (0,0),(1,1),(2,1) remain.
    assert gpu.image(0, 0, 3, 2) == [[31, 0, 0], [0, 31, 31]]


def test_zero_display_ranges_are_black_one_pixel_png(gpu, tmp_path):
    gpu.gp1(0x06000000)
    gpu.gp1(0x07000000)
    path = tmp_path / "blank.png"
    assert gpu.lib.host_display_dump(gpu.g, os.fsencode(path))
    assert read_png(path) == (1, 1, bytes(3))


def test_display_area_write_protection_and_override(gpu):
    gpu.display(4, 2, x=4, y=4)
    gpu.gp0(0x600000FF, xy(3, 3), xy(4, 4))
    assert gpu.image(3, 3, 4, 4) == [[31]*4, [31, 0, 0, 0], [31, 0, 0, 0], [31]*4]
    gpu.gp0(0xE1000400, 0x6800FF00, xy(4, 4))
    assert gpu.pixel(4, 4) == 992
    gpu.gp0(0xE1000000)
    gpu.gp1(0x03000001)
    gpu.gp0(0x680000FF, xy(4, 4))
    assert gpu.pixel(4, 4) == 31


def test_interlaced_rendering_field_and_draw_to_display(gpu):
    gpu.display(4, 2)
    gpu.gp1(0x08000024)
    gpu.gp0(0x600000FF, xy(0, 0), xy(2, 4))
    assert gpu.image(0, 0, 2, 4) == [[0, 0], [31, 31], [0, 0], [31, 31]]
    gpu.lib.gpu_vblank(gpu.g)
    gpu.gp0(0x6000FF00, xy(0, 0), xy(2, 4))
    assert gpu.image(0, 0, 2, 4) == [[992, 992], [31, 31], [992, 992], [31, 31]]
    gpu.gp0(0xE1000400, 0x60FF0000, xy(0, 0), xy(2, 4))
    assert gpu.image(0, 0, 2, 4) == [[0x7C00, 0x7C00]] * 4


@pytest.mark.parametrize("chunk", [1, 2, 3, 7, 255])
def test_dma_packets_match_word_port(gpu_lib, chunk):
    # Image upload, odd pixel count, partial commands, and a Gouraud polyline
    # all cross arbitrary DMA packet boundaries.
    words = [0xe3000000, 0xe407ffff, 0xa0000000, 0x00080010, 0x00010003,
             0x03e0001f, 0x00007c00, 0x020000ff, 0x00200010, 0x00020010,
             0x580000ff, 0x00300020, 0x0000ff00, 0x00340024,
             0x00ff0000, 0x00380028, 0x50005000, 0xe6000001]
    packed = struct.pack("<" + "I" * len(words), *words)
    left, right = gpu_lib.gpu_create(), gpu_lib.gpu_create()
    try:
        for word in words:
            gpu_lib.gpu_gp0(left, word)
        for start in range(0, len(words), chunk):
            data = packed[start * 4:(start + chunk) * 4]
            gpu_lib.gpu_gp0_words(right, (C.c_uint8 * len(data)).from_buffer_copy(data), len(data) // 4)
        gpu_lib.gpu_vram.argtypes = [C.c_void_p]
        gpu_lib.gpu_vram.restype = C.POINTER(C.c_uint16)
        assert C.string_at(gpu_lib.gpu_vram(left), 1024 * 512 * 2) == C.string_at(gpu_lib.gpu_vram(right), 1024 * 512 * 2)
        assert gpu_lib.gpu_status(left) == gpu_lib.gpu_status(right)
    finally:
        gpu_lib.test_gpu_free(left)
        gpu_lib.test_gpu_free(right)
