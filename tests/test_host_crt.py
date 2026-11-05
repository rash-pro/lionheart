"""CRT presentation presets: options, shaders, and synthetic patterns on Vulkan."""
import ctypes as C
import json
import os
from pathlib import Path
import re
import shlex
import shutil
import struct
import subprocess

import numpy as np
import pytest

ROOT = Path(__file__).resolve().parents[1]
SHADERS = ROOT / 'backends/host/shaders'
SHADER_NAMES = ['crt_fullscreen.vert', 'crt_lines.frag', 'crt_beam.frag', 'crt_glow.frag', 'crt_final.frag']

PRESETS = ('pvm', 'tv', 'arcade')
# 320x240 NTSC progressive: GP1(06h) 600..3160, GP1(07h) 16..256, GP1(08h) mode 1.
TIMING = (600 | (3160 << 12), 16 | (256 << 10), 1)
NATIVE = (320, 240)
BRIGHTNESS_TOLERANCE = 0.08  # linear mean of a 50% grey field, central region, versus off


def spirv_words(path):
    return [int(word, 16) for word in re.findall(r'0x[0-9a-fA-F]+', path.read_text())]


@pytest.mark.parametrize('name', SHADER_NAMES)
def test_prebuilt_shader_matches_source_and_validates(tmp_path, name):
    prebuilt = SHADERS / 'prebuilt' / f'{name}.inc'
    words = spirv_words(prebuilt)
    assert words[0] == 0x07230203 and len(words) > 5
    spv = tmp_path / 'shader.spv'
    spv.write_bytes(struct.pack(f'<{len(words)}I', *words))
    if shutil.which('spirv-val'):
        subprocess.run(['spirv-val', str(spv)], check=True, capture_output=True)
    glslc = shutil.which('glslc')
    if not glslc:
        pytest.skip('glslc is not installed; prebuilt SPIR-V only validated')
    rebuilt = tmp_path / f'{name}.inc'
    subprocess.run([glslc, '-O', '-mfmt=c', '-o', str(rebuilt), str(SHADERS / name)], check=True, cwd=SHADERS)
    recorded = (SHADERS / 'prebuilt' / 'GLSLC_VERSION').read_text()
    local = subprocess.run([glslc, '--version'], check=True, capture_output=True, text=True).stdout
    if local != recorded:
        pytest.skip('prebuilt SPIR-V comes from another glslc build; source compiled and prebuilt validated')
    assert rebuilt.read_text() == prebuilt.read_text(), f'run backends/host/shaders/rebuild.sh after editing {name}'


@pytest.fixture(scope='module')
def options_binary(tmp_path_factory):
    output = tmp_path_factory.mktemp('crt-options') / 'options'
    sdl = shlex.split(subprocess.check_output(['pkg-config', '--cflags', '--libs', 'sdl2'], text=True))
    subprocess.run(['cc', '-std=c99', '-Wall', '-Wextra', '-Werror', '-Ibackends/host',
                    'tests/host_options_harness.c', 'backends/host/options.c', '-o', str(output), *sdl], cwd=ROOT, check=True)
    return output


def run_options(binary, *args, **env):
    return subprocess.run([str(binary), 'unused.exe', *args, '--config', os.devnull], text=True, capture_output=True,
                          env={**os.environ, 'HOME': str(binary.parent), 'XDG_CONFIG_HOME': str(binary.parent / 'xdg'),
                               'FF8_TEST_CRT': '1', **env})


def test_crt_option_parsing_and_classification(options_binary):
    result = run_options(options_binary)
    assert result.returncode == 0 and result.stdout.splitlines()[-1] == 'crt=0 present=1920x1080 presented='
    classes = dict(line.split() for line in result.stdout.splitlines()[1:-1])
    assert classes['crt'] == 'P' and classes['dump-presented-frames'] == 'P' and classes['present-size'] == 'P'
    for index, name in enumerate(('off', *PRESETS)):
        result = run_options(options_binary, '--crt', name, '--present-size', '3840x2160', '--dump-presented-frames', '/tmp/x')
        assert result.returncode == 0 and result.stdout.splitlines()[-1] == f'crt={index} present=3840x2160 presented=/tmp/x'
    ini = options_binary.parent / 'crt.ini'
    ini.write_text('crt=tv\npresent-size=1280x960\n')
    result = subprocess.run([str(options_binary), 'unused.exe', '--config', str(ini)], text=True, capture_output=True,
                            env={**os.environ, 'FF8_TEST_CRT': '1'})
    assert result.returncode == 0 and result.stdout.splitlines()[-1] == 'crt=2 present=1280x960 presented='
    result = subprocess.run([str(options_binary), 'unused.exe', '--config', str(ini), '--crt', 'off'], text=True, capture_output=True,
                            env={**os.environ, 'FF8_TEST_CRT': '1'})
    assert result.stdout.splitlines()[-1].startswith('crt=0 ')


@pytest.mark.parametrize('args', [['--crt'], ['--crt', 'lcd'], ['--crt', 'PVM'], ['--present-size', '1920'],
                                  ['--present-size', '8x8'], ['--present-size', '1920x1080x2'], ['--present-size', '9000x100']])
def test_crt_invalid_settings(options_binary, args):
    assert run_options(options_binary, *args).returncode == 2


class Rect(C.Structure):
    _fields_ = [(name, C.c_int) for name in ('x', 'y', 'w', 'h')]


class View(C.Structure):
    _fields_ = [('frame', Rect), ('content', Rect)]


class Timing(C.Structure):
    _fields_ = [(name, C.c_uint32) for name in ('horizontal', 'vertical', 'mode')]


@pytest.fixture(scope='module')
def viewport(tmp_path_factory):
    path = tmp_path_factory.mktemp('crt-viewport') / 'viewport.so'
    subprocess.run(['cc', '-std=c99', '-Wall', '-Wextra', '-Werror', '-shared', '-fPIC', '-Iruntime',
                    'backends/host/viewport.c', '-o', str(path)], cwd=ROOT, check=True)
    lib = C.CDLL(str(path))
    lib.host_viewport.argtypes = [Timing] + [C.c_int] * 5
    lib.host_viewport.restype = View

    def rects(timing, size, native=NATIVE, mode=0):
        view = lib.host_viewport(Timing(*timing), native[0], native[1], size[0], size[1], mode)
        return ((view.frame.x, view.frame.y, view.frame.w, view.frame.h),
                (view.content.x, view.content.y, view.content.w, view.content.h))
    return rects


@pytest.fixture(scope='module')
def crt_preview(tmp_path_factory, vulkan_replay):
    if not shutil.which('cmake'):
        pytest.skip('CMake is required for the CRT presentation gate')
    out = tmp_path_factory.mktemp('crt')
    generated = out / 'gen'
    generated.mkdir()
    (generated / 'sources.cmake').write_text('set(GENERATED_SOURCES "")\n')
    (generated / 'manifest.json').write_text('{}\n')
    subprocess.run(['cmake', '-S', str(ROOT), '-B', str(out / 'host'), f'-DGENERATED_DIR={generated}', '-DFF8_PLAYER=OFF',
                    '-DFF8_VULKAN=ON', '-DCMAKE_BUILD_TYPE=Release'], check=True, capture_output=True, timeout=120)
    subprocess.run(['cmake', '--build', str(out / 'host'), '--target', 'crt_preview', '-j4'],
                   check=True, capture_output=True, timeout=300)
    return out / 'host/crt_preview'


def grey_field(level=128):
    return bytes([level]) * (NATIVE[0] * NATIVE[1] * 3)


def present(tool, rgb, size, preset, out_dir, timing=TIMING, filt='nearest', scale_mode='aspect', scale=1, native=NATIVE):
    src, dst = out_dir / f'{preset}-{size[0]}x{size[1]}-in.rgb', out_dir / f'{preset}-{size[0]}x{size[1]}-out.rgb'
    src.write_bytes(rgb)
    assert len(rgb) == native[0] * native[1] * scale * scale * 3
    run = subprocess.run([str(tool), str(src), str(native[0] * scale), str(native[1] * scale), '--scale', str(scale),
                          '--native', str(native[0]), str(native[1]), '--timing', *map(str, timing),
                          '--size', f'{size[0]}x{size[1]}', '--crt', preset, '--filter', filt, '--scale-mode', scale_mode,
                          '--out', str(dst)],
                         env=dict(os.environ, SDL_VIDEODRIVER='offscreen', SDL_AUDIODRIVER='dummy'),
                         text=True, capture_output=True, timeout=300)
    assert run.returncode == 0, run.stderr
    report = json.loads(run.stdout)
    assert (report['width'], report['height']) == tuple(size)
    return dst.read_bytes()


def image(rgb, size):
    return np.frombuffer(rgb, np.uint8).reshape(size[1], size[0], 3)


def to_linear(values):
    v = values / 255.0
    return np.where(v <= 0.04045, v / 12.92, ((v + 0.055) / 1.055) ** 2.4)


def region_mean_linear(rgb, size, rect):
    x0, y0, w, h = rect
    return float(to_linear(image(rgb, size)[y0:y0 + h, x0:x0 + w].astype(float)).mean())


def row_profile(rgb, size, rect):
    x0, y0, w, h = rect
    return image(rgb, size)[y0:y0 + h, x0:x0 + w].astype(float).mean(axis=(1, 2))


def dominant_cycles(profile):
    spectrum = np.abs(np.fft.rfft(profile - profile.mean()))
    spectrum[0] = 0
    return int(spectrum.argmax())


def test_off_presentation_is_the_nearest_blit(crt_preview, viewport, tmp_path):
    """Integer 4x into 1280x960: a nearest blit is driver-independent at that ratio."""
    size = (1280, 960)
    frame, content = viewport(TIMING, size)
    assert frame == (0, 0, 1280, 960) and content == (0, 0, 1280, 960)
    pattern = bytearray()
    for y in range(NATIVE[1]):
        for x in range(NATIVE[0]):
            pattern += bytes(((x * 7) & 255, (y * 3) & 255, ((x ^ y) * 5) & 255))
    out = present(crt_preview, bytes(pattern), size, 'off', tmp_path)
    expected = bytearray()
    for y in range(size[1]):
        row = pattern[(y // 4) * NATIVE[0] * 3:(y // 4 + 1) * NATIVE[0] * 3]
        expected += b''.join(row[(x // 4) * 3:(x // 4) * 3 + 3] for x in range(size[0]))
    assert out == bytes(expected)
    size = (1920, 1080)
    frame, content = viewport(TIMING, size)
    assert frame == (240, 0, 1440, 1080) and content == frame
    out = image(present(crt_preview, bytes(pattern), size, 'off', tmp_path), size)
    assert not out[:, :240].any() and not out[:, 1680:].any() and out[:, 240:1680].any(axis=(1, 2)).all()


@pytest.mark.parametrize('preset', PRESETS)
@pytest.mark.parametrize('size', [(1920, 1080), (1280, 960), (3840, 2160)])
def test_preset_scanlines_borders_brightness_and_determinism(crt_preview, viewport, tmp_path, preset, size):
    frame, content = viewport(TIMING, size)
    grey = grey_field(128)
    out = present(crt_preview, grey, size, preset, tmp_path)
    assert out == present(crt_preview, grey, size, preset, tmp_path), 'presentation must be deterministic'
    reference = present(crt_preview, grey, size, 'off', tmp_path)
    # Beam geometry follows the native line count: the row profile over the
    # content area has exactly NATIVE[1] cycles.
    x0, y0, w, h = content
    inner = (x0 + w // 4, y0 + h // 8, w // 2, h - h // 4)
    profile = row_profile(out, size, inner)
    cycles = dominant_cycles(profile)
    assert abs(cycles / (len(profile) / h) - NATIVE[1]) <= 1, (preset, size, cycles)
    # Black outside the TV frame, including the letterbox at 16:9.
    pixels = image(out, size)
    mask = np.ones((size[1], size[0]), bool)
    mask[frame[1]:frame[1] + frame[3], frame[0]:frame[0] + frame[2]] = False
    assert not pixels[mask].any()
    # Brightness of a 50% grey field stays within tolerance of off (central
    # region, so the TV presets' vignette and rounded corners are excluded).
    centre = (x0 + w // 4, y0 + h // 4, w // 2, h // 2)
    ratio = region_mean_linear(out, size, centre) / region_mean_linear(reference, size, centre)
    assert abs(ratio - 1) <= BRIGHTNESS_TOLERANCE, (preset, size, ratio)


def test_presets_draw_inside_the_scanout_rectangle(crt_preview, viewport, tmp_path):
    """A shorter, shifted display range leaves black inside the frame around the content."""
    size = (1920, 1080)
    timing = (600 | (3160 << 12), 40 | (232 << 10), 1)  # 192 lines starting 24 lines down
    native = (320, 192)
    frame, content = viewport(timing, size, native)
    assert frame == (240, 0, 1440, 1080) and content[1] > frame[1] and content[1] + content[3] < frame[1] + frame[3]
    for preset in PRESETS:
        out = image(present(crt_preview, bytes([200]) * (native[0] * native[1] * 3), size, preset, tmp_path,
                            timing=timing, native=native), size)
        lit = np.flatnonzero(out.any(axis=(1, 2)))
        assert len(lit) and lit[0] >= content[1] - 1 and lit[-1] < content[1] + content[3] + 1, (preset, lit[0], lit[-1])


def test_interlaced_scanout_uses_thinner_beams(crt_preview, viewport, tmp_path):
    """480 interlaced lines at 1080p: the beam period follows the doubled line count."""
    size = (1920, 1080)
    timing = (600 | (3160 << 12), 16 | (256 << 10), 1 | 4 | 32)
    native = (320, 480)
    frame, content = viewport(timing, size, native)
    assert content == frame
    out = present(crt_preview, bytes([128]) * (native[0] * native[1] * 3), size, 'pvm', tmp_path, timing=timing, native=native)
    x0, y0, w, h = content
    profile = row_profile(out, size, (x0 + w // 4, y0 + h // 8, w // 2, h - h // 4))
    cycles = dominant_cycles(profile)
    assert abs(cycles / (len(profile) / h) - native[1]) <= 2, cycles
