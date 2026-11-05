"""Controller chords on the guest wire and asynchronous presented screenshots."""
import ctypes as C
import os
from pathlib import Path
import shlex
import struct
import subprocess
import zlib

import numpy as np
import pytest

from test_host_crt import crt_preview, viewport, TIMING

ROOT = Path(__file__).resolve().parents[1]
ENV = {**os.environ, 'SDL_VIDEODRIVER': 'dummy', 'SDL_AUDIODRIVER': 'dummy', 'SDL_RENDER_DRIVER': 'software', 'TZ': 'UTC'}


def compile_c(out, sources, *flags):
    sdl = shlex.split(subprocess.check_output(['pkg-config', '--cflags', '--libs', 'sdl2'], text=True))
    subprocess.run(['cc', '-std=c99', '-O1', '-g', '-Wall', '-Wextra', '-Werror', '-Iruntime', '-Ibackends/host',
                    *sources, *flags, '-o', str(out), *sdl, '-lz'], cwd=ROOT, check=True)


SOURCES = ['backends/host/display.c', 'backends/host/screenshot.c', 'backends/host/osd.c', 'backends/host/viewport.c']


def test_controller_hotkeys_guest_wire(tmp_path):
    out = tmp_path / 'hotkeys'
    flags = ['-fsanitize=address,undefined', '-fno-omit-frame-pointer'] if os.environ.get('FF8_TEST_SANITIZE') else []
    compile_c(out, ['tests/hotkeys_harness.c', *SOURCES, 'backends/host/input.c',
                    *map(str, sorted((ROOT / 'runtime').glob('*.c')))], '-Wl,--wrap=host_osd_event,--wrap=host_osd_post,--wrap=SDL_CreateWindow', *flags)
    result = subprocess.run([str(out), str(tmp_path / 'shots')], check=True, env=ENV, timeout=60,
                            capture_output=True, text=True)
    assert 'Disc 2 missing; skipping empty slot' in result.stderr
    assert 'Disc 3 missing; skipping empty slot' in result.stderr
    assert len(list((tmp_path / 'shots').glob('*.png'))) == 2


@pytest.fixture(scope='module')
def soft_capture(tmp_path_factory):
    out = tmp_path_factory.mktemp('screen-soft') / 'capture'
    compile_c(out, ['tests/screenshot_host.c', *SOURCES, 'runtime/gpu.c', 'runtime/gpu_raster_soft.c'],
              '-Wl,--wrap=host_osd_event,--wrap=time')
    return out


@pytest.fixture(scope='module')
def vulkan_capture(crt_preview):
    subprocess.run(['cmake', '--build', str(crt_preview.parent), '--target', 'screenshot_capture', '-j4'],
                   check=True, capture_output=True, timeout=180)
    return crt_preview.parent / 'screenshot_capture'


def png(path):
    data = path.read_bytes()
    assert data[:8] == b'\x89PNG\r\n\x1a\n'
    pos = 8; compressed = b''
    while pos < len(data):
        size, = struct.unpack_from('>I', data, pos)
        kind = data[pos+4:pos+8]; chunk = data[pos+8:pos+8+size]
        if kind == b'IHDR':
            w, h, depth, color, *_ = struct.unpack('>IIBBBBB', chunk)
            assert (depth, color) == (8, 2)
        elif kind == b'IDAT':
            compressed += chunk
        pos += 12 + size
    raw = np.frombuffer(zlib.decompress(compressed), dtype=np.uint8).reshape(h, w*3+1)
    assert set(raw[:, 0].tolist()) <= {0, 2}
    rows = raw[:, 1:].copy()
    for y in range(1, h):
        if raw[y, 0] == 2: rows[y] += rows[y-1]
    return rows.reshape(h, w, 3)


@pytest.mark.parametrize('backend,preset', [('soft', 0), ('soft', 2), ('vulkan', 0), ('vulkan', 2)])
@pytest.mark.parametrize('size', [(1920, 1080), (3840, 2160)])
@pytest.mark.parametrize('mode', [0, 2])
@pytest.mark.parametrize('linear', [0, 1])
def test_screenshot_is_presented_crop_without_osd(request, viewport, tmp_path, backend, preset, size, mode, linear):
    executable = request.getfixturevalue(f'{backend}_capture')
    subprocess.run([str(executable), backend, *map(str, size), str(mode), str(preset), str(linear), str(tmp_path), '0'],
                   check=True, capture_output=True, env=ENV, timeout=90)
    baseline = tmp_path / 'no-osd'
    subprocess.run([str(executable), backend, *map(str, size), str(mode), str(preset), str(linear), str(baseline), '2'],
                   check=True, capture_output=True, env=ENV, timeout=90)
    reference = png(baseline / 'presented/frame-000001.png')
    with_osd = png(tmp_path / 'presented/frame-000001.png')
    assert not np.array_equal(reference, with_osd)
    # This fixture uses 224 active rows inside the full 240-line TV aperture.
    timing = (TIMING[0], 16 | (240 << 10), TIMING[2])
    (x, y, w, h), _ = viewport(timing, size, native=(320, 224), mode=mode)
    files = sorted((tmp_path / 'shots').glob('*.png'))
    assert len(files) == 2
    assert {p.name for p in files} == {'ff8-20231114-221320.png', 'ff8-20231114-221320-2.png'}
    for file in files:
        assert np.array_equal(png(file), reference[y:y+h, x:x+w])


def test_unwritable_directory_reports_failure(soft_capture, tmp_path):
    subprocess.run([str(soft_capture), 'soft', '1920', '1080', '0', '0', '0', str(tmp_path), '1'],
                   check=True, env=ENV, capture_output=True, timeout=30)
    assert not (tmp_path / 'shots').exists()


def test_hotkey_options_and_classification(tmp_path):
    out = tmp_path / 'options'
    compile_c(out, ['tests/host_options_harness.c', 'backends/host/options.c'])
    def run(*args):
        return subprocess.run([str(out), 'unused', '--config', os.devnull, *args], env={**ENV, 'FF8_TEST_HOTKEYS': '1'},
                              text=True, capture_output=True)
    result = run()
    assert result.returncode == 0 and result.stdout.endswith('hotkey=0 directory= frames=')
    assert all(f'{key} P\n' in result.stdout for key in ('pad-hotkey', 'screenshot-dir', 'screenshot-at'))
    assert run('--pad-hotkey', 'off', '--screenshot-dir', '/tmp/screens', '--screenshot-at', '0', '--screenshot-at', '20').stdout.endswith(
        'hotkey=1 directory=/tmp/screens frames=0\n20\n')
    for args in [('--pad-hotkey', 'home'), ('--screenshot-at', '-1'), ('--screenshot-at', '1x'),
                 ('--screenshot-at', '18446744073709551616'), ('--screenshot-at',), ('--pad-hotkey',), ('--screenshot-dir',)]:
        assert run(*args).returncode == 2
    ini = tmp_path / 'ff8.ini'; ini.write_text('pad-hotkey=off\nscreenshot-dir=/ini\nscreenshot-at=3\n')
    assert run('--config', str(ini), '--pad-hotkey', 'guide').stdout.endswith('hotkey=0 directory=/ini frames=3\n')


@pytest.fixture(scope='module')
def directory_lib(tmp_path_factory):
    out = tmp_path_factory.mktemp('screen-dir') / 'dir.so'
    compile_c(out, ['backends/host/screenshot.c', 'backends/host/osd.c'], '-fPIC', '-shared')
    lib = C.CDLL(str(out)); lib.host_screenshot_directory.argtypes = [C.c_char_p, C.c_uint]
    return lib


@pytest.mark.parametrize('source', ['fallback', 'env', 'config', 'home-config', 'escaped', 'malicious'])
def test_xdg_resolution(directory_lib, monkeypatch, tmp_path, source):
    home = tmp_path / 'home'; config = tmp_path / 'config'; home.mkdir(); config.mkdir()
    monkeypatch.setenv('HOME', str(home)); monkeypatch.setenv('XDG_CONFIG_HOME', str(config)); monkeypatch.delenv('XDG_PICTURES_DIR', raising=False)
    expected = home / 'Pictures/FF8'
    if source == 'env':
        monkeypatch.setenv('XDG_PICTURES_DIR', str(tmp_path / 'photos')); expected = tmp_path / 'photos/FF8'
    elif source == 'home-config':
        monkeypatch.delenv('XDG_CONFIG_HOME'); (home / '.config').mkdir(); config = home / '.config'
    if source in ('config', 'home-config', 'escaped'):
        value = '$HOME/My Photos' if source != 'escaped' else '$HOME/My \\"Photos\\"'
        (config / 'user-dirs.dirs').write_text(f'XDG_PICTURES_DIR="{value}"\n')
        expected = home / ('My Photos/FF8' if source != 'escaped' else 'My "Photos"/FF8')
    if source == 'malicious':
        (config / 'user-dirs.dirs').write_text('XDG_PICTURES_DIR="$(touch /tmp/host-must-not-execute)"\n')
    buffer = C.create_string_buffer(4096)
    assert directory_lib.host_screenshot_directory(buffer, len(buffer))
    assert buffer.value.decode() == str(expected)


def test_worker_is_async_and_queue_is_bounded(tmp_path):
    out = tmp_path / 'queue'
    compile_c(out, ['tests/screenshot_queue_harness.c', 'backends/host/screenshot.c', 'backends/host/osd.c'],
              '-Wl,--wrap=host_osd_event')
    subprocess.run([str(out), str(tmp_path / 'shots')], check=True, env=ENV, timeout=30)
    files = list((tmp_path / 'shots').glob('*.png'))
    assert len(files) == 2
    for file in files:
        assert png(file).tolist() == [[[255, 0, 0], [0, 255, 0]], [[0, 0, 255], [255, 255, 255]]]
