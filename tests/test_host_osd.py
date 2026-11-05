"""Presentation-only OSD: copy, cells, cached pixels, deterministic clock and composition."""
import ctypes as C
import os
from pathlib import Path
import shlex
import subprocess
import sys

import numpy as np
import pytest

from test_host_crt import Rect, crt_preview, present, TIMING

ROOT = Path(__file__).resolve().parents[1]


class Viewport(C.Structure):
    _fields_ = [('frame', Rect), ('content', Rect)]


class Panel(C.Structure):
    _fields_ = [('rgba', C.POINTER(C.c_uint8))] + [(k, C.c_int) for k in ('x', 'y', 'w', 'h', 'cw', 'ch')] + [('revision', C.c_uint), ('opacity', C.c_uint8)]


@pytest.fixture(scope='module')
def osd(tmp_path_factory):
    out = tmp_path_factory.mktemp('osd') / 'osd.so'
    sdl = shlex.split(subprocess.check_output(['pkg-config', '--cflags', '--libs', 'sdl2'], text=True))
    subprocess.run(['cc', '-std=c99', '-Wall', '-Wextra', '-Werror', '-fPIC', '-shared', '-Iruntime', '-Ibackends/host',
                    'backends/host/osd.c', '-o', str(out), *sdl], cwd=ROOT, check=True)
    lib = C.CDLL(str(out))
    lib.host_osd_configure.argtypes = [C.c_int, C.c_char_p]
    lib.host_osd_clock.argtypes = [C.c_double]
    lib.host_osd_post.argtypes = [C.c_char_p, C.c_char_p, C.c_int]
    lib.host_osd_event.argtypes = [C.c_uint, C.c_uint, C.c_char_p]
    lib.host_osd_panel.argtypes = [C.c_int, C.c_int, Rect]
    lib.host_osd_panel.restype = C.POINTER(Panel)
    lib.host_osd_area.argtypes = [Viewport]
    lib.host_osd_area.restype = Rect
    return lib


@pytest.mark.parametrize('frame,content,area', [
    ((240, 0, 1440, 1080), (240, 36, 1440, 1008), (240, 36, 1440, 1008)),  # 224 lines in a 240-line aperture
    ((240, 0, 1440, 1080), (200, -20, 1520, 1120), (240, 0, 1440, 1080)),  # crop: picture clipped to the frame
    ((0, 0, 1920, 1080), (0, 0, 0, 0), (0, 0, 1920, 1080)),                # no picture: fall back to the frame
])
def test_area_is_the_visible_picture(osd, frame, content, area):
    r = osd.host_osd_area(Viewport(Rect(*frame), Rect(*content)))
    assert (r.x, r.y, r.w, r.h) == area


def post(lib, label='CRT', value='TV', persistent=0):
    lib.host_osd_post(label.encode(), value.encode(), persistent)


def panel(lib, size=(1920, 1080), frame=None):
    p = lib.host_osd_panel(*size, Rect(*(frame or (0, 0, *size))))
    if not p:
        return None, None
    p = p.contents
    return p, np.ctypeslib.as_array(p.rgba, shape=(p.h, p.w, 4)).copy()


def reset(lib):
    lib.host_osd_configure(1, None)
    lib.host_osd_clock(0)


def test_font_header_matches_bdfs(tmp_path):
    out = tmp_path / 'font.h'
    subprocess.run([sys.executable, 'tools/spleen_header.py', '--output', str(out)], cwd=ROOT, check=True)
    assert out.read_bytes() == (ROOT / 'backends/host/osd_font.h').read_bytes()


@pytest.mark.parametrize('size,height,width', [((1280, 720), 16, 8), ((1920, 1080), 24, 12), ((2560, 1440), 32, 16), ((3840, 2160), 48, 24), ((5120, 2880), 64, 32)])
def test_cells_geometry_colors_and_cache(osd, size, height, width):
    reset(osd); post(osd); osd.host_osd_clock(120)
    p, pixels = panel(osd, size, (101, 42, 900, 600))
    assert (p.ch, p.cw) == (height, width)
    assert (p.x, p.y) == (119, 60)
    assert (p.w, p.h) == (6 * width + 2 * round(.75 * height), height + 2 * int(.4 * height + .5))
    colors = set(map(tuple, pixels.reshape(-1, 4)))
    assert (16, 16, 20, 199) in colors
    assert (165, 165, 175, 255) in colors and (242, 242, 242, 255) in colors
    assert pixels[0, 0, 3] == 0
    assert any(0 < a < 199 for r, g, b, a in colors)  # antialiased corners
    revision = p.revision
    osd.host_osd_clock(200)
    q, other = panel(osd, size)
    assert q.revision == revision and np.array_equal(pixels, other)
    q, _ = panel(osd, (size[0] + 1, size[1]))
    assert q.revision > revision


def test_utf8_latin1_and_fallback(osd):
    reset(osd); post(osd, 'éñÅ', ''); osd.host_osd_clock(120)
    p, latin = panel(osd)
    assert p.w == 3 * 12 + 36
    post(osd, '???', ''); _, fallback = panel(osd)
    assert not np.array_equal(latin, fallback)
    post(osd, '界🙂€', ''); _, unknown = panel(osd)
    assert np.array_equal(unknown, fallback)
    assert (165, 165, 175, 255) not in set(map(tuple, unknown.reshape(-1, 4)))


def test_clock_replacement_and_persistence(osd):
    reset(osd); post(osd)
    assert panel(osd)[0] is None
    for ms, alpha in [(60, 128), (120, 255), (1919, 255), (1920, 255), (2070, 128)]:
        osd.host_osd_clock(ms); assert panel(osd)[0].opacity == alpha
    post(osd, 'State saved', 'slot 3')
    assert panel(osd)[0].opacity == 255
    osd.host_osd_clock(2070 + 1920); assert panel(osd)[0].opacity == 255
    osd.host_osd_clock(2070 + 2220); assert panel(osd)[0] is None
    post(osd, 'Saving', 'slot 3...', 1)
    osd.host_osd_clock(100000); assert panel(osd)[0].opacity == 255
    osd.host_osd_event(13, 0, None)  # Fast forward replaces immediately
    assert panel(osd)[0].opacity == 255
    osd.host_osd_clear(3); assert panel(osd)[0] is not None
    osd.host_osd_clear(13); assert panel(osd)[0] is None
    osd.host_osd_configure(0, None); post(osd); osd.host_osd_clock(120)
    assert panel(osd)[0] is None


def test_deterministic_diagnostic(osd):
    osd.host_osd_configure(1, b'3:CRT:TV\n100:State saved:slot 3\n')
    osd.host_osd_frame(0, 2); assert panel(osd)[0] is None
    osd.host_osd_frame(0, 3); assert panel(osd)[0] is None
    osd.host_osd_frame(0, 7); assert 140 <= panel(osd)[0].opacity <= 143
    osd.host_osd_frame(0, 11); assert panel(osd)[0].opacity == 255
    osd.host_osd_frame(0, 100); assert panel(osd)[0].opacity == 255
    osd.host_osd_frame(0, 234); assert panel(osd)[0] is None


COPY = [
    (0, 0, None, 'CRT', 'Off'), (0, 1, None, 'CRT', 'PVM'), (0, 2, None, 'CRT', 'TV'), (0, 3, None, 'CRT', 'Arcade'),
    (1, 0, None, 'CRT', 'needs the Vulkan renderer'), (2, 3, None, 'State slot', '3'), (2, 3, b'empty', 'State slot', '3 (empty)'),
    (3, 3, None, 'Saving', 'slot 3...'), (4, 3, None, 'State saved', 'slot 3'), (5, 3, b'timed out', 'Save failed', 'slot 3: timed out'),
    (6, 3, None, 'Loading', 'slot 3...'), (7, 3, None, 'State loaded', 'slot 3'), (8, 3, None, 'No state', 'in slot 3'),
    (9, 3, None, 'Load failed', 'slot 3'), (10, 0, None, 'Memory card', 'changed: reopen the save menu'),
    (11, 0, None, 'RA strict', 'loading states is off'), (12, 0, None, 'RA strict', 'fast-forward is off'),
    (13, 0, None, 'Fast forward', ''), (14, 2, None, 'Disc', '2'), (15, 0, 'Pad é'.encode(), 'Controller', 'connected: Pad é'),
    (16, 0, None, 'Controller', 'disconnected'), (17, 0, None, 'Save states', 'not available'),
    (18, 0, None, 'Screenshot', 'saved'), (19, 0, b'cannot write', 'Screenshot failed', 'cannot write')]


@pytest.mark.parametrize('event,number,detail,label,value', COPY)
def test_event_copy(osd, event, number, detail, label, value):
    reset(osd); osd.host_osd_event(event, number, detail); osd.host_osd_clock(120)
    _, actual = panel(osd)
    post(osd, label, value); _, expected = panel(osd)
    assert np.array_equal(actual, expected)


def test_osd_options(tmp_path):
    out = tmp_path / 'options'
    sdl = shlex.split(subprocess.check_output(['pkg-config', '--cflags', '--libs', 'sdl2'], text=True))
    subprocess.run(['cc', '-std=c99', '-Wall', '-Wextra', '-Werror', '-Ibackends/host', 'tests/host_options_harness.c',
                    'backends/host/options.c', '-o', str(out), *sdl], cwd=ROOT, check=True)
    env = {**os.environ, 'FF8_TEST_OSD': '1'}
    def run(*args):
        return subprocess.run([str(out), 'unused', '--config', os.devnull, *args], env=env, text=True, capture_output=True)
    r = run(); assert r.returncode == 0 and r.stdout.endswith('osd=1 tests=')
    assert 'osd P\n' in r.stdout and 'osd-test P\n' in r.stdout
    r = run('--osd', '0', '--osd-test', '3:CRT:TV', '--osd-test', '20:Fast forward')
    assert r.returncode == 0 and r.stdout.endswith('osd=0 tests=3:CRT:TV\n20:Fast forward\n')
    for args in [('--osd',), ('--osd', '2'), ('--osd-test', 'x:X'), ('--osd-test', '-1:X'), ('--osd-test', '1:'), ('--osd-test', '1::X'), ('--osd-test', '18446744073709551616:X')]:
        assert run(*args).returncode == 2
    env['FF8_TEST_RA'] = '1'
    env.pop('FF8_TEST_RA')
    ini = tmp_path / 'ff8.ini'; ini.write_text('osd=0\nosd-test=2:CRT:TV\n')
    r = run('--config', str(ini)); assert r.stdout.endswith('osd=0 tests=2:CRT:TV\n')


@pytest.fixture(scope='module')
def soft_preview(tmp_path_factory):
    out = tmp_path_factory.mktemp('soft-osd') / 'soft'
    sdl = shlex.split(subprocess.check_output(['pkg-config', '--cflags', '--libs', 'sdl2'], text=True))
    subprocess.run(['cc', '-std=c99', '-Wall', '-Wextra', '-Werror', '-Iruntime', '-Ibackends/host',
                    'tests/osd_soft_host.c', 'backends/host/display.c','backends/host/screenshot.c', 'backends/host/osd.c', 'backends/host/viewport.c',
                    'runtime/gpu.c', 'runtime/gpu_raster_soft.c',
                    '-Wl,--wrap=SDL_CreateWindow,--wrap=SDL_RenderPresent,--wrap=host_osd_frame', '-o', str(out), *sdl, '-lz'], cwd=ROOT, check=True)
    return out


@pytest.mark.parametrize('size,age', [((1280,720),120), ((1920,1080),60), ((1920,1080),120), ((1920,1080),2070), ((2560,1440),120), ((3840,2160),120)])
def test_soft_vulkan_composition(crt_preview, soft_preview, tmp_path, size, age):
    width, height = size
    src = tmp_path / 'in.rgb'; src.write_bytes(bytes([132]) * (320 * 240 * 3))
    vk = tmp_path / 'vk.rgb'; soft = tmp_path / 'soft.rgb'
    label, value = 'Controller', 'connected: Pad é'
    command = [str(crt_preview), str(src), '320', '240', '--scale', '1', '--native', '320', '240',
               '--timing', *map(str,TIMING), '--size', f'{width}x{height}', '--crt', 'off', '--out', str(vk),
               '--filter', 'nearest', '--osd-label', label, '--osd-value', value, '--osd-age', str(age)]
    subprocess.run(command, cwd=ROOT, check=True, capture_output=True,
                   env={**os.environ, 'SDL_VIDEODRIVER':'dummy', 'SDL_AUDIODRIVER':'dummy'})
    subprocess.run([str(soft_preview),str(width),str(height),str(soft),label,value,str(age)], check=True, capture_output=True,
                   env={**os.environ,'SDL_VIDEODRIVER':'dummy','SDL_AUDIODRIVER':'dummy','SDL_RENDER_DRIVER':'software'})
    a = np.frombuffer(vk.read_bytes(), dtype=np.uint8).astype(int)
    b = np.frombuffer(soft.read_bytes(), dtype=np.uint8).astype(int)
    assert abs(a-b).max() <= 2


def test_osd_shader_matches_prebuilt(tmp_path):
    from test_host_crt import test_prebuilt_shader_matches_source_and_validates
    test_prebuilt_shader_matches_source_and_validates(tmp_path, 'osd.frag')


def test_sdl_osd_event_hooks(tmp_path):
    out = tmp_path / 'input'
    sdl = shlex.split(subprocess.check_output(['pkg-config', '--cflags', '--libs', 'sdl2'], text=True))
    subprocess.run(['cc','-std=c99','-Wall','-Wextra','-Werror','-Iruntime','-Ibackends/host',
                    'tests/osd_input_harness.c','backends/host/states_disabled.c','backends/host/display.c','backends/host/screenshot.c','backends/host/osd.c','backends/host/viewport.c',
                    'runtime/gpu.c','runtime/gpu_raster_soft.c','-Wl,--wrap=host_osd_event','-o',str(out),*sdl,'-lz'],cwd=ROOT,check=True)
    subprocess.run([str(out)],check=True,env={**os.environ,'SDL_VIDEODRIVER':'dummy','SDL_AUDIODRIVER':'dummy'},timeout=30)


def test_vulkan_sdl_osd_events(crt_preview):
    subprocess.run(['cmake','--build',str(crt_preview.parent),'--target','osd_input','-j4'],check=True,capture_output=True,timeout=120)
    subprocess.run([str(crt_preview.parent/'osd_input'),'vulkan'],check=True,capture_output=True,timeout=30,
                   env={**os.environ,'SDL_VIDEODRIVER':'dummy','SDL_AUDIODRIVER':'dummy'})
