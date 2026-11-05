"""Synthetic IROS, timestamp decoding, compositing and guest-read contracts."""
import json
import os
from pathlib import Path
import struct
import subprocess
import tempfile

import pytest

from tools.iro_extract import directory, extract
from test_vulkan import vulkan_env


def archive(path, name='CRF\\movies\\disc00_00h.avi', flags=0, payload=b'owned-avi'):
    name = name.encode('utf-16-le')
    length = len(name) + 20
    path.write_bytes(struct.pack('<4sIIIIHH', b'IROS', 0x10002, 0, 16, 1, length, len(name)) +
                     name + struct.pack('<IQI', flags, 20 + length, len(payload)) + payload)


@pytest.fixture
def external_cache():
    # The extractor deliberately refuses repository destinations, including build/.
    with tempfile.TemporaryDirectory(prefix="ff8-movie-cache-") as path:
        yield Path(path)


def test_iro_directory_and_atomic_extraction(external_cache):
    tmp_path = external_cache
    source = tmp_path / 'owned.iroj'
    archive(source)
    entries = directory(source)
    assert entries[0].name == 'CRF/movies/disc00_00h.avi'
    output = tmp_path / 'cache'
    assert extract(source, output, entries) == 1
    assert (output / 'disc00_00h.avi').read_bytes() == b'owned-avi'
    assert extract(source, output, entries) == 1
    (output / 'disc00_00h.avi').write_bytes(b'owned-avj')
    assert extract(source, output, entries) == 1
    assert (output / 'disc00_00h.avi').read_bytes() == b'owned-avi'


@pytest.mark.parametrize('bad', ['../escape.avi', '/escape.avi', 'C:\\escape.avi'])
def test_iro_rejects_paths(tmp_path, bad):
    source = tmp_path / 'bad.iroj'
    archive(source, bad)
    with pytest.raises(ValueError, match='unsafe'):
        directory(source)


def test_iro_rejects_unknown_compression_before_extracting(external_cache):
    tmp_path = external_cache
    source = tmp_path / 'compressed.iroj'
    archive(source, flags=1)
    output = tmp_path / 'cache'
    with pytest.raises(ValueError, match='compressed'):
        extract(source, output, directory(source))
    assert not output.exists()
    source.write_bytes(source.read_bytes()[:-1])
    with pytest.raises(ValueError, match='beyond'):
        directory(source)


@pytest.mark.parametrize('rgb24,scale,msaa', [(0,1,1),(1,1,1),(0,4,1),(1,4,1),(0,4,4),(1,4,4)])
def test_hd_upload_keeps_native_reads_and_draw_order(vulkan_replay, tmp_path, rgb24, scale, msaa):
    host = vulkan_replay.parent
    subprocess.run(['cmake', '--build', str(host), '--target', 'movie_contract', '-j4'], check=True, capture_output=True, timeout=120)
    # A 30 fps clip tests guest 15 fps -> decoded presentation timestamps.
    subprocess.run(['ffmpeg', '-nostdin', '-v', 'error', '-f', 'lavfi', '-i', 'color=0x00ff00:size=640x448:rate=30',
                    '-t', '1', '-an', '-vf', 'scale=out_color_matrix=bt709', '-colorspace', 'bt709',
                    '-c:v', 'ffv1', '-f', 'matroska', str(tmp_path / 'disc00_00h.avi')], check=True, timeout=30)
    run = subprocess.run([str(host / 'movie_contract'), str(tmp_path), str(rgb24), str(tmp_path / 'stats.csv'), str(scale), str(msaa)],
                         env=vulkan_env(),
                         text=True, capture_output=True, timeout=60)
    assert run.returncode == 0, (run.returncode, run.stdout, run.stderr)
    assert json.loads(run.stdout)['native_reads_exact']
    assert 'ready' in (tmp_path / 'stats.csv').read_text()


def test_missing_hd_asset_leaves_original_available(vulkan_replay, tmp_path):
    host = vulkan_replay.parent
    subprocess.run(['cmake', '--build', str(host), '--target', 'movie_contract', '-j4'], check=True, capture_output=True, timeout=120)
    run = subprocess.run([str(host / 'movie_contract'), str(tmp_path), '2', str(tmp_path / 'stats.csv')],
                         env=vulkan_env(),
                         text=True, capture_output=True, timeout=30)
    assert run.returncode == 0, run.stderr
    assert json.loads(run.stdout)['missing_falls_back']
    assert 'missing' in (tmp_path / 'stats.csv').read_text()


def test_missing_leading_source_frame_uses_original(vulkan_replay, tmp_path):
    host = vulkan_replay.parent
    subprocess.run(['cmake', '--build', str(host), '--target', 'movie_contract', '-j4'], check=True, capture_output=True, timeout=120)
    subprocess.run(['ffmpeg', '-nostdin', '-v', 'error', '-f', 'lavfi', '-i', 'color=green:size=640x448:rate=30',
                    '-t', '1', '-an', '-c:v', 'ffv1', str(tmp_path / 'disc01_00h.avi')], check=True, timeout=30)
    run = subprocess.run([str(host / 'movie_contract'), str(tmp_path), '3', str(tmp_path / 'stats.csv')],
                         env=vulkan_env(),
                         text=True, capture_output=True, timeout=30)
    assert run.returncode == 0, (run.returncode, run.stderr)
    assert json.loads(run.stdout)['negative_offset_fallback']
    assert 'missing' in (tmp_path / 'stats.csv').read_text()
