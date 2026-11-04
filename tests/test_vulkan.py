"""Optional real Vulkan contracts on lavapipe; no disc or display server required."""
import csv
import json
import os
from pathlib import Path
import shutil
import struct
import subprocess

import pytest
from test_gpu import read_png

ROOT = Path(__file__).resolve().parents[1]
ICD = Path('/usr/share/vulkan/icd.d/lvp_icd.json')


@pytest.fixture(scope='module')
def vulkan_replay(tmp_path_factory):
    if not ICD.is_file() or not shutil.which('cmake'):
        pytest.skip('lavapipe and CMake are required for the optional Vulkan gate')
    out = tmp_path_factory.mktemp('vulkan')
    generated = out / 'gen'
    generated.mkdir()
    (generated / 'sources.cmake').write_text('set(GENERATED_SOURCES "")\n')
    (generated / 'manifest.json').write_text('{}\n')
    subprocess.run(['cmake', '-S', str(ROOT), '-B', str(out / 'host'),
                    f'-DGENERATED_DIR={generated}', '-DFF8_PLAYER=OFF', '-DFF8_VULKAN=ON',
                    '-DCMAKE_BUILD_TYPE=Release', '-DFF8_VULKAN_TESTING=ON', '-DFF8_PGXP=ON'], check=True, capture_output=True, timeout=120)
    subprocess.run(['cmake', '--build', str(out / 'host'), '--target', 'gpu_replay', 'gpu_wsi_contract', '-j4'],
                   check=True, capture_output=True, timeout=240)
    return out / 'host/gpu_replay'


@pytest.mark.parametrize('scale,color,msaa', [(1,0,1), (2,0,1), (3,0,1), (4,0,1), (5,0,1), (6,0,1), (7,0,1), (8,0,1), (4,1,4), (1,1,1), (1,0,4)])
def test_vulkan_reference_readbacks_and_scaled_scanout(vulkan_replay, tmp_path, scale, color, msaa):
    png = tmp_path / 'scanout.png'
    csv = tmp_path / 'timing.csv'
    env = dict(os.environ, VK_DRIVER_FILES=str(ICD), SDL_VIDEODRIVER='offscreen', SDL_AUDIODRIVER='dummy')
    run = subprocess.run([str(vulkan_replay), '--self-test', str(scale), str(png), str(csv), str(color), str(msaa)],
                         env=env, text=True, capture_output=True, timeout=120)
    assert run.returncode == 0, run.stderr
    report = json.loads(run.stdout)
    assert report['errors'] == 0 and report['reads'] == 24
    if scale == 4 and not color and msaa == 1:
        assert report['shape_checks'] == 2
    if scale == 1 and not color and msaa == 1:
        assert report['raw_mismatch_pixels'] == 0
    data = png.read_bytes()
    assert data[:8] == b'\x89PNG\r\n\x1a\n'
    assert struct.unpack('>II', data[16:24]) == (320*scale, 240*scale)
    width, height, rgb = read_png(png)
    for x,y,expected in [(5,5,(16,24,41)), (21,5,(24,33,49)),
                      (15,100,(255,0,255)), (15,108,(255,255,0))]:
        at = ((y*scale+scale//2)*width+x*scale+scale//2)*3
        if not color:
            assert tuple(rgb[at:at+3]) == expected
    assert len(csv.read_text().splitlines()) == 10


def test_vulkan_replay_rejects_truncated_stream(vulkan_replay, tmp_path):
    stream = tmp_path / 'bad.gpu'
    stream.write_bytes(b'FF8GPUR1' + b'\x00')
    result = subprocess.run([str(vulkan_replay), str(stream), '1'],
                            env=dict(os.environ, VK_DRIVER_FILES=str(ICD)), capture_output=True, timeout=60)
    assert result.returncode == 2


@pytest.mark.parametrize('scale', [1, 4])
def test_vulkan_pipeline_keeps_guest_reads_native(vulkan_replay, tmp_path, scale):
    """Read the native oracle while three frames of GPU work may be outstanding."""
    timing = tmp_path / 'async.csv'
    run = subprocess.run([str(vulkan_replay), '--self-test-async', str(scale),
                          str(tmp_path / 'async.png'), str(timing), '0', '1'],
                         env=dict(os.environ, VK_DRIVER_FILES=str(ICD)),
                         text=True, capture_output=True, timeout=120)
    assert run.returncode == 0, run.stderr
    report = json.loads(run.stdout)
    assert report['errors'] == 0 and report['reads'] == 24
    if scale == 1:
        assert report['raw_mismatch_pixels'] == 0
    rows = list(csv.DictReader(timing.open()))
    assert len(rows) == 9  # Includes the final explicit capture/verification frame.
    assert all(float(row['gpu_ms']) >= 0 for row in rows)  # Deferred queries drain at shutdown.
    for row in rows[1:8]:
        assert float(row['capture_ms']) == 0
        assert int(row['submissions']) == 1


def test_vulkan_wsi_recovery_and_retirement(vulkan_replay):
    run = subprocess.run([str(vulkan_replay.with_name('gpu_wsi_contract'))],
                         text=True, capture_output=True, timeout=30)
    assert run.returncode == 0, run.stderr
    assert json.loads(run.stdout)['checks'] >= 40


@pytest.mark.parametrize('phase', [1, 2, 3, 4, 5])
def test_vulkan_c_boundaries_keep_native_state_after_failure(vulkan_replay, tmp_path, phase):
    run = subprocess.run([str(vulkan_replay), '--self-test-fault', str(phase),
                          str(tmp_path / 'unused.png'), str(tmp_path / 'fault.csv')],
                         env=dict(os.environ, VK_DRIVER_FILES=str(ICD)),
                         text=True, capture_output=True, timeout=120)
    assert run.returncode == 0, run.stderr
    assert json.loads(run.stdout)['fallback'] == 'pass'
    assert 'falling back to soft' in run.stderr


@pytest.mark.parametrize('override', ['missing-driver', 'invalid-device'])
def test_vulkan_initialization_falls_back_to_soft(vulkan_replay, tmp_path, override):
    env = dict(os.environ, VK_DRIVER_FILES=str(ICD))
    if override == 'missing-driver':
        env['VK_DRIVER_FILES'] = str(tmp_path / 'not-an-icd.json')
    else:
        env['GRANITE_VULKAN_DEVICE_INDEX'] = '9999'
    shot = tmp_path / 'native.png'
    run = subprocess.run([str(vulkan_replay), '--self-test-fallback', '4', str(shot)],
                         env=env, text=True, capture_output=True, timeout=120)
    assert run.returncode == 0, run.stderr
    assert json.loads(run.stdout)['fallback'] == 'pass'
    assert 'soft presentation' in run.stderr
    assert struct.unpack('>II', shot.read_bytes()[16:24]) == (320, 240)


def test_vulkan_unavailable_window_uses_sdl_soft_renderer(vulkan_replay, tmp_path):
    run = subprocess.run([str(vulkan_replay), '--self-test-fallback-window', '4', str(tmp_path / 'fallback.png')],
                         env=dict(os.environ, SDL_VIDEODRIVER='offscreen', SDL_AUDIODRIVER='dummy',
                                  VK_DRIVER_FILES=str(tmp_path / 'missing-icd.json')),
                         text=True, capture_output=True, timeout=120)
    assert run.returncode == 0, run.stderr
    assert json.loads(run.stdout)['fallback'] == 'pass'


def test_vulkan_synchronization_validation_when_installed(vulkan_replay, tmp_path):
    layers = list(Path('/usr/share/vulkan/explicit_layer.d').glob('*khronos*validation*.json'))
    layers += list(Path('/etc/vulkan/explicit_layer.d').glob('*khronos*validation*.json'))
    if not layers:
        pytest.skip('VK_LAYER_KHRONOS_validation is not installed')
    run = subprocess.run([str(vulkan_replay), '--self-test-async', '4', str(tmp_path / 'sync.png')],
                         env=dict(os.environ, VK_DRIVER_FILES=str(ICD),
                                  VK_INSTANCE_LAYERS='VK_LAYER_KHRONOS_validation',
                                  VK_LAYER_ENABLES='VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT'),
                         text=True, capture_output=True, timeout=120)
    assert run.returncode == 0, run.stderr
    assert 'SYNC-HAZARD' not in run.stderr and 'Validation Error' not in run.stderr


@pytest.mark.parametrize('scale,msaa', [(1, 1), (4, 1), (4, 4)])
@pytest.mark.parametrize('mask_test', [False, True])
def test_vram_copy_force_mask_is_used_by_textured_blending(vulkan_replay, tmp_path, scale, msaa, mask_test):
    """A framebuffer copy sets bit 15 before the copied image is used as an effect texture."""
    words = []

    def gp0(*values):
        words.extend((0, value) for value in values)

    def xy(x, y):
        return x | (y << 16)

    gp0(0xe3000000, 0xe4000000 | 1023 | (511 << 10), 0xe1000400)
    # Render blue into VRAM, then copy to a texture page with force-mask enabled.
    gp0(0x60400000, xy(640, 256), xy(32, 32))
    gp0(0xe6000000 | 1 | (2 if mask_test else 0))
    gp0(0x80000000, xy(640, 256), xy(704, 256), xy(32, 32))
    gp0(0xe6000000)
    # Add the copied blue texture to a red background. Bit 15 selects blending.
    gp0(0x60000040, xy(0, 0), xy(32, 32), 0xe100053b)
    gp0(0x66808080, xy(0, 0), 0, xy(32, 32))
    words.extend((1, value) for value in [0x05000000, 0x06000000 | 600 | (3160 << 12),
                                        0x07000000 | 16 | (256 << 10), 0x08000001, 0x03000000])
    words.append((2, 0))
    stream = tmp_path / 'copy-mask.gpu'
    stream.write_bytes(b'FF8GPUR1' + b''.join(struct.pack('<II', *word) for word in words))
    png = tmp_path / 'copy-mask.png'
    result = subprocess.run([str(vulkan_replay), str(stream), str(scale), str(png),
                             str(tmp_path / 'copy-mask.csv'), '0', str(msaa), '--benchmark'],
                            env=dict(os.environ, VK_DRIVER_FILES=str(ICD)), capture_output=True, text=True, timeout=120)
    assert result.returncode == 0, result.stderr
    assert json.loads(result.stdout)['errors'] == 0
    width, _, rgb = read_png(png)
    at = ((8 * scale) * width + 8 * scale) * 3
    red, green, blue = rgb[at:at + 3]
    assert red >= 50 and blue >= 50 and green == 0, (red, green, blue)


@pytest.mark.parametrize('scale,msaa', [(1, 1), (4, 1), (4, 4)])
def test_framebuffer_feedback_preserves_15bit_truncation(vulkan_replay, tmp_path, scale, msaa):
    """Repeated framebuffer feedback must not accumulate discarded fractional color bits."""
    words = []

    def gp0(*values):
        words.extend((0, value) for value in values)

    def xy(x, y):
        return x | (y << 16)

    words.extend((1, value) for value in [0x05000000, 0x06000000 | 600 | (3160 << 12),
                                        0x07000000 | 16 | (256 << 10), 0x08000001, 0x03000000])
    gp0(0xe3000000, 0xe4000000 | 1023 | (511 << 10), 0xe1000400)
    gp0(0x60000008, xy(0, 0), xy(320, 240))
    gp0(0xe6000001, 0x80000000, xy(0, 0), xy(640, 256), xy(320, 240), 0xe6000000)
    for _ in range(16):
        # 1 * 32 / 16 >> 3 == 0 in PS1 color arithmetic; red must remain 1.
        gp0(0xe100053a, 0x66202020, xy(0, 0), 0, xy(320, 240))
        gp0(0x80000000, xy(0, 0), xy(640, 256), xy(320, 240))
        words.append((2, 0))
    stream = tmp_path / 'feedback.gpu'
    stream.write_bytes(b'FF8GPUR1' + b''.join(struct.pack('<II', *word) for word in words))
    png = tmp_path / 'feedback.png'
    result = subprocess.run([str(vulkan_replay), str(stream), str(scale), str(png),
                             str(tmp_path / 'feedback.csv'), '0', str(msaa), '--benchmark'],
                            env=dict(os.environ, VK_DRIVER_FILES=str(ICD)), capture_output=True, text=True, timeout=120)
    assert result.returncode == 0, result.stderr
    assert json.loads(result.stdout)['errors'] == 0
    width, _, rgb = read_png(png)
    at = ((8 * scale) * width + 8 * scale) * 3
    assert tuple(rgb[at:at + 3]) == (8, 0, 0)


def test_upload_releases_feedback_texture_for_scaled_geometry(vulkan_replay, tmp_path):
    words = []

    def gp0(*values):
        words.extend((0, value) for value in values)

    def xy(x, y):
        return x | (y << 16)

    words.extend((1, value) for value in [0x05000000, 0x06000000 | 600 | (3160 << 12),
                                        0x07000000 | 16 | (256 << 10), 0x08000001, 0x03000000])
    gp0(0xe3000000, 0xe4000000 | 1023 | (511 << 10), 0xe1000400)
    gp0(0x60000008, xy(0, 0), xy(320, 240), 0xe6000001)
    gp0(0x80000000, xy(0, 0), xy(640, 256), xy(320, 240), 0xe6000000)
    # Reuse the texture pages for an ordinary uploaded texture.
    gp0(0xa0000000, xy(640, 256), xy(256, 256))
    gp0(*([0x001f001f] * (256 * 256 // 2)))
    gp0(0x02000000, xy(0, 0), xy(320, 240))
    gp0(0x25808080, xy(0, 0), 0, xy(8, 0), 0x11a << 16, xy(0, 8), 0)
    words.append((2, 0))
    stream = tmp_path / 'reuse.gpu'
    stream.write_bytes(b'FF8GPUR1' + b''.join(struct.pack('<II', *word) for word in words))
    png = tmp_path / 'reuse.png'
    result = subprocess.run([str(vulkan_replay), str(stream), '4', str(png),
                             str(tmp_path / 'reuse.csv'), '0', '1', '--benchmark'],
                            env=dict(os.environ, VK_DRIVER_FILES=str(ICD)), capture_output=True, text=True, timeout=120)
    assert result.returncode == 0, result.stderr
    assert json.loads(result.stdout)['errors'] == 0
    width, _, rgb = read_png(png)
    edge = [rgb[((4 * 4 + y) * width + 3 * 4 + x) * 3] for y in range(4) for x in range(4)]
    assert min(edge) == 0 and max(edge) > 200  # Subpixel edge coverage has resumed.


@pytest.mark.parametrize('scale,msaa', [(1, 1), (4, 1), (4, 4)])
def test_hud_background_copy_keeps_scaled_pixels(vulkan_replay, tmp_path, scale, msaa):
    """HUD save/restore copies must retain scaled pixels after framebuffer feedback."""
    words = []

    def gp0(*values):
        words.extend((0, value) for value in values)

    def xy(x, y):
        return x | (y << 16)

    words.extend((1, value) for value in [0x05000000, 0x06000000 | 600 | (3160 << 12),
                                        0x07000000 | 16 | (256 << 10), 0x08000001, 0x03000000])
    gp0(0xe3000000, 0xe4000000 | 1023 | (511 << 10), 0xe1000400)
    # An earlier transition tagged this framebuffer as a 15-bit feedback source.
    gp0(0xe6000001, 0x80000000, xy(640, 256), xy(0, 0), xy(320, 240), 0xe6000000)
    gp0(0x200000f8, xy(0, 0), xy(16, 0), xy(0, 16))
    words.append((2, 0))
    # Battle saves the background under its HUD to scratch VRAM, then restores it
    # on the following VBlank. This is a copy, not another feedback color operation.
    gp0(0xe6000001, 0x80000000, xy(0, 0), xy(384, 256), xy(16, 16))
    gp0(0x80000000, xy(384, 256), xy(0, 0), xy(16, 16))
    words.append((2, 0))
    stream = tmp_path / 'hud-copy.gpu'
    stream.write_bytes(b'FF8GPUR1' + b''.join(struct.pack('<II', *word) for word in words))
    frames = tmp_path / 'frames'
    frames.mkdir()
    result = subprocess.run([str(vulkan_replay), str(stream), str(scale), str(tmp_path / 'last.png'),
                             str(tmp_path / 'timing.csv'), '0', str(msaa), '--benchmark',
                             '--dump-scaled-frames', str(frames)],
                            env=dict(os.environ, VK_DRIVER_FILES=str(ICD)), capture_output=True, text=True, timeout=120)
    assert result.returncode == 0, result.stderr
    assert json.loads(result.stdout)['errors'] == 0
    assert read_png(frames / 'frame-000001.png') == read_png(frames / 'frame-000002.png')
