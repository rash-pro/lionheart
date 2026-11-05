"""Owned GPU presentation versions, including commands after a display swap."""
import json
import os
from pathlib import Path
import shutil
import struct
import subprocess

import pytest
from test_gpu import read_png

ROOT = Path(__file__).resolve().parents[1]


@pytest.fixture(scope='module')
def journal_host(tmp_path_factory):
    icd = Path(os.environ.get('FF8_TEST_VULKAN_ICD', '/usr/share/vulkan/icd.d/nvidia_icd.json'))
    if not icd.exists() or not shutil.which('cmake'):
        pytest.skip('Vulkan driver and CMake required for presentation versions')
    supplied = os.environ.get('FF8_PRESENTATION_TEST_HOST')
    if supplied:
        return Path(supplied), icd
    out = tmp_path_factory.mktemp('presentation-build')
    generated = out / 'gen'
    generated.mkdir()
    (generated / 'sources.cmake').write_text('set(GENERATED_SOURCES "")\n')
    (generated / 'manifest.json').write_text('{}\n')
    subprocess.run(['cmake', '-S', str(ROOT), '-B', str(out / 'host'),
                    f'-DGENERATED_DIR={generated}', '-DFF8_VULKAN=ON', '-DFF8_PGXP=ON',
                    '-DFF8_FRAME_PROVENANCE=ON', '-DFF8_GUI=OFF', '-DFF8_PLAYER=OFF',
                    '-DFF8_RCHEEVOS=OFF', '-DCMAKE_BUILD_TYPE=Release'],
                   check=True, capture_output=True, timeout=120)
    subprocess.run(['cmake', '--build', str(out / 'host'), '--target', 'gpu_replay', 'frame_interpolation_contract', '-j4'],
                   check=True, capture_output=True, timeout=240)
    return out / 'host/gpu_replay', icd


def construction_stream():
    words = []

    def gp0(*data):
        words.extend((0, value) for value in data)

    def gp1(value):
        words.append((1, value))

    def xy(x, y):
        return x | (y << 16)

    def upload(x, y, value):
        gp0(0xa0000000, xy(x, y), xy(2, 1), value)

    gp1(0x06000000 | 600 | (3160 << 12))
    gp1(0x07000000 | 16 | (256 << 10))
    gp1(0x08000001)
    gp1(0x03000000)
    gp0(0xe3000000, 0xe407ffff, 0xe1000400)
    upload(640, 300, 0x03e0001f)
    for frame in range(6):
        x = 512 if frame % 2 == 0 else 0
        gp0(0x02000000, xy(x, 0), xy(320, 240))
        gp0(0x200000f8, xy(x + 10 + frame, 10), xy(x + 80, 20), xy(x + 20, 70))
        # A texture version is consumed before its same-address replacement.
        gp0(0xe100051a, 0x65000000, xy(x + 100, 20), 44 << 8, xy(2, 1))
        upload(640, 300, 0x7c007fff if frame % 2 else 0x03e0001f)
        gp1(0x05000000 | x)
        # Current front-buffer completion, followed by a VRAM save/restore.
        gp0(0xe1000400, 0x6000f800, xy(x + 10, 200), xy(32, 8))
        gp0(0x80000000, xy(x + 10, 200), xy(900, 400), xy(32, 8))
        words.append((2, 0))
        gp0(0x60000000, xy(x + 10, 200), xy(32, 8))
        gp0(0x80000000, xy(900, 400), xy(x + 10, 200), xy(32, 8))
        words.append((2, 0))
    return b'FF8GPUR1' + b''.join(struct.pack('<II', *w) for w in words)


@pytest.mark.parametrize('scale,msaa', [(1, 1), (4, 1), (4, 4)])
def test_native_replay_versions_textures_copies_and_current_hud(journal_host, tmp_path, scale, msaa):
    binary, icd = journal_host
    stream = tmp_path / 'commands.gpu'
    stream.write_bytes(construction_stream())
    results = []
    for enabled in (False, True):
        folder = tmp_path / ('journal' if enabled else 'native')
        folder.mkdir()
        env = {k: v for k, v in os.environ.items() if not k.startswith('FF8_')}
        env.update(VK_DRIVER_FILES=str(icd), SDL_VIDEODRIVER='offscreen', SDL_AUDIODRIVER='dummy')
        if enabled:
            env['FF8_PRESENTATION_REPLAY'] = '1'
        result = subprocess.run([str(binary), str(stream), str(scale), str(folder / 'last.png'),
                                 str(folder / 'timing.csv'), '0', str(msaa), '--benchmark',
                                 '--dump-scaled-frames', str(folder)], env=env, capture_output=True,
                                text=True, timeout=120)
        assert result.returncode == 0, result.stderr
        assert json.loads(result.stdout)['errors'] == 0
        assert 'falling back' not in result.stderr
        results.append({p.name: p.read_bytes() for p in folder.glob('*.png')})
    assert len(results[0]) == 13
    assert results[0] == results[1]


@pytest.mark.parametrize('unknown', range(8), ids=[
    'interpolate', 'unknown-origin', 'fullscreen-fade', 'fixed-background', 'scroll', 'background-color',
    'background-texture-version', 'inconsistent-background-scroll'])
def test_live_temporal_positions_preserve_guest_and_current_hud(journal_host, tmp_path, unknown):
    binary, icd = journal_host
    binary = binary.with_name('frame_interpolation_contract')
    outputs = []
    for enabled in (False, True):
        folder = tmp_path / ('on' if enabled else 'off')
        folder.mkdir()
        env = {k: v for k, v in os.environ.items() if not k.startswith('FF8_')}
        env.update(VK_DRIVER_FILES=str(icd), SDL_VIDEODRIVER='offscreen', SDL_AUDIODRIVER='dummy')
        result = subprocess.run([str(binary), str(int(enabled)), str(folder), str(int(unknown))],
                                env=env, capture_output=True, text=True, timeout=120)
        assert result.returncode == 0, result.stderr
        assert 'falling back' not in result.stderr
        outputs.append(folder)
    assert (outputs[0] / 'guest.bin').read_bytes() == (outputs[1] / 'guest.bin').read_bytes()
    centroids = [[], []]
    for frame in range(16):
        pictures = [read_png(folder / f'frame-{frame:02d}.png') for folder in outputs]
        for width, height, rgb in pictures:
            assert (width, height) == (1280, 960)
        for row in range(320, 352):
            start = row * 1280 * 3
            assert pictures[0][2][start:start + 32 * 3] == pictures[1][2][start:start + 32 * 3]
        if unknown not in (0, 3, 4):
            assert pictures[0][2] == pictures[1][2]
        else:
            for n, (_, _, rgb) in enumerate(pictures):
                xs = [x for y in range(100, 240) for x in range(100, 400)
                      if rgb[(y * 1280 + x) * 3] > 200 and rgb[(y * 1280 + x) * 3 + 1] < 20]
                centroids[n].append(sum(xs) / len(xs) / 4)
    if unknown in (0, 3, 4):
        assert centroids[0][4:8] == [44] * 4
        assert centroids[1][4:8] == ([38, 39.5, 41, 42.5] if unknown==4 else [36, 38, 40, 42])
        assert centroids[1][8] == centroids[0][4] + (2 if unknown==4 else 0)
