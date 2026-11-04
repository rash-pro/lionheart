"""TV geometry from GP1 timing: fixed line pitch, borders, and clipping."""
import ctypes as C
from pathlib import Path
import subprocess

import pytest

ROOT = Path(__file__).resolve().parents[1]


class Rect(C.Structure):
    _fields_ = [(name, C.c_int) for name in ("x", "y", "w", "h")]

    def values(self):
        return self.x, self.y, self.w, self.h


class View(C.Structure):
    _fields_ = [("frame", Rect), ("content", Rect)]


class Timing(C.Structure):
    _fields_ = [(name, C.c_uint32) for name in ("horizontal", "vertical", "mode")]


@pytest.fixture(scope="module")
def viewport(tmp_path_factory):
    path = tmp_path_factory.mktemp("viewport") / "viewport.so"
    subprocess.run(["cc", "-std=c99", "-Wall", "-Wextra", "-Werror", "-shared", "-fPIC", "-Iruntime",
                    "backends/host/viewport.c", "-o", str(path)], cwd=ROOT, check=True)
    lib = C.CDLL(str(path))
    lib.host_viewport.argtypes = [Timing] + [C.c_int] * 5
    lib.host_viewport.restype = View
    return lib.host_viewport


def timing(mode, x1, x2, y1, y2):
    return Timing(x1 | (x2 << 12), y1 | (y2 << 10), mode)


# Mode, actual pixels, GP1 horizontal endpoints, expected X and width in 1080p.
# 368 pixels occupy 2576 clocks inside the mode's 2688-clock TV aperture.
MODES = [(0, 256, 590, 3150, 240, 1440, 320, 1280),
         (1, 320, 600, 3160, 240, 1440, 320, 1280),
         (64, 368, 595, 3171, 270, 1380, 347, 1226),
         (2, 512, 615, 3175, 240, 1440, 320, 1280),
         (3, 640, 620, 3180, 240, 1440, 320, 1280)]


@pytest.mark.parametrize("mode,width,x1,x2,ax,aw,ix,iw", MODES)
@pytest.mark.parametrize("height,interlace,y1,y2,ay,ah,iy,ih", [
    (224, 0, 24, 248, 36, 1008, 92, 896), (240, 0, 16, 256, 0, 1080, 60, 960),
    (448, 36, 24, 248, 36, 1008, 92, 896), (480, 36, 16, 256, 0, 1080, 60, 960)])
def test_tv_geometry(viewport, mode, width, x1, x2, ax, aw, ix, iw,
                     height, interlace, y1, y2, ay, ah, iy, ih):
    s = timing(mode | interlace, x1, x2, y1, y2)
    aspect = viewport(s, width, height, 1920, 1080, 0)
    assert aspect.frame.values() == (240, 0, 1440, 1080)
    assert aspect.content.values() == (ax, ay, aw, ah)
    integer = viewport(s, width, height, 1920, 1080, 1)
    assert integer.frame.values() == (320, 60, 1280, 960)
    assert integer.content.values() == (ix, iy, iw, ih)
    assert integer.content.h % height == 0
    stretch = viewport(s, width, height, 1920, 1080, 2)
    assert stretch.content.values() == (0, 0, 1920, 1080)


def test_narrow_and_shifted_ranges(viewport):
    v = viewport(timing(1, 760, 3000, 24, 248), 280, 224, 1920, 1080, 0)
    assert v.content.values() == (330, 36, 1260, 1008)
    for start, expected_x in [(680, 285), (520, 195)]:
        v = viewport(timing(1, start, start + 2560, 16, 256), 320, 240, 1920, 1080, 0)
        assert v.frame.values() == (240, 0, 1440, 1080)
        assert v.content.values() == (expected_x, 0, 1440, 1080)
    v = viewport(timing(64, 539, 3227, 16, 256), 384, 240, 1920, 1080, 0)
    assert v.content.values() == v.frame.values() == (240, 0, 1440, 1080)


def test_short_range_retains_tv_line_pitch(viewport):
    short = viewport(timing(1, 600, 3160, 24, 248), 320, 224, 1920, 1080, 0)
    full = viewport(timing(1, 600, 3160, 16, 256), 320, 240, 1920, 1080, 0)
    assert short.content.h / 224 == full.content.h / 240 == 4.5


def test_crop_preserves_pixel_aspect(viewport):
    v = viewport(timing(1, 600, 3160, 24, 248), 320, 224, 1920, 1080, 3)
    assert v.frame.values() == v.content.values() == (189, 0, 1542, 1080)


def test_pal_and_small_window(viewport):
    s = timing(9, 624, 3184, 35, 291)
    assert viewport(s, 320, 256, 1920, 1080, 0).content.values() == (240, 60, 1440, 960)
    assert viewport(s, 320, 256, 1920, 1080, 1).content.values() == (384, 156, 1152, 768)
    s = timing(37, 600, 3160, 24, 248)
    v = viewport(s, 320, 448, 200, 100, 1)
    assert v.frame.values() == (33, 0, 133, 100)
    assert v.content.values() == (33, 3, 133, 94)
    assert viewport(s, 0, 448, 200, 100, 0).content.values() == (0, 0, 0, 0)
