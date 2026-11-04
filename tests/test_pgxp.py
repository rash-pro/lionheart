"""Clean-room projection-cache and GPU replay behavior, without disc fixtures."""
import os
from pathlib import Path
import subprocess
import pytest

ROOT = Path(__file__).resolve().parents[1]


def test_pgxp_cache_and_parser(tmp_path):
    binary = tmp_path / 'pgxp'
    subprocess.run(['cc', '-std=c99', '-O2', '-Wall', '-Wextra', '-Wpedantic', '-Werror',
                    '-fsanitize=undefined', '-fno-sanitize-recover=all', '-DFF8_PGXP', '-Iruntime',
                    'tests/pgxp_harness.c', 'runtime/pgxp.c', 'runtime/gpu.c', 'runtime/gpu_raster_soft.c',
                    '-Wl,--wrap=gpu_raster_triangle', '-o', str(binary)], cwd=ROOT, check=True)
    run = subprocess.run([str(binary)], text=True, capture_output=True, check=True)
    assert 'contracts passed' in run.stdout


# Reuse the real Vulkan build for projection/perspective pixel contracts.
from test_vulkan import vulkan_replay, ICD
from test_gpu import read_png
import json
import struct


def xy(x, y):
    return (x & 65535) | ((y & 65535) << 16)


def projection_stream(path, *, gouraud=False, missing_fourth=False):
    data = bytearray(b'FF8GPUR2')

    def row(kind, value):
        data.extend(struct.pack('<II', kind, value))

    def gp0(*words):
        for word in words:
            row(0, word)

    row(1, 0x03000000)
    gp0(0xE3000000, 0xE407FFFF, 0xE1000400)
    # Direct-color texture outside the display buffer, an observable U/V ramp.
    gp0(0xA0000000, xy(512, 256), xy(64, 64))
    pixels = [(x//2) | ((y//2) << 5) | (16 << 10) for y in range(64) for x in range(64)]
    gp0(*(pixels[i] | (pixels[i+1] << 16) for i in range(0, len(pixels), 2)))
    vertices = [(20,20,100), (180,20,400), (20,180,200)]
    for x,y,z in vertices:
        row(7, xy(x,y))
        data.extend(struct.pack('<qqI', x*65536+49152, y*65536+32768, z))
    if gouraud:
        gp0(0x300000FF, xy(20,20), 0x0000FF00, xy(180,20), 0x00FF0000, xy(20,180))
    else:
        gp0(0x2D808080 if missing_fourth else 0x25808080, xy(20,20), 0,
            xy(180,20), (0x118 << 16) | 63, xy(20,180), 63 << 8)
        if missing_fourth:
            gp0(xy(180,180), (63 << 8) | 63)
    # Guest-visible read of the rendered region is identical with all modes.
    gp0(0xC0000000, xy(60,60), xy(2,1))
    # Native raster samples integer positions: UV=15.75 -> texel (15,15).
    native = 7 | (7 << 5) | (16 << 10)
    if not gouraud:
        row(3, native | ((native+1) << 16))
    row(2, 0)
    path.write_bytes(data)


def replay(vulkan_replay, stream, shot, mode, scale=4, policy=None):
    command=[str(vulkan_replay), str(stream), str(scale), str(shot),
             str(shot.with_suffix('.csv')), '0', '1', '--pgxp', mode]
    if policy: command += ['--pgxp-policy',policy]
    run = subprocess.run(command,
                         env=dict(os.environ, VK_DRIVER_FILES=str(ICD)),
                         text=True, capture_output=True, timeout=120)
    assert run.returncode == 0, run.stderr
    report = json.loads(run.stdout)
    assert report['errors'] == 0
    return report


def test_precise_vertices_and_reciprocal_depth_texture_pixels(vulkan_replay, tmp_path):
    stream = tmp_path / 'plane.gpu'
    projection_stream(stream)
    images = {}
    for mode in ('off', 'vertex', 'perspective'):
        shot = tmp_path / f'{mode}.png'
        report = replay(vulkan_replay, stream, shot, mode)
        assert report['reads'] == 1
        if mode != 'off':
            assert report['pgxp']['precise'] == report['pgxp']['candidates'] == 1
        images[mode] = read_png(shot)
    assert images['off'][2] != images['vertex'][2] != images['perspective'][2]
    # At (60.625,60.625), affine U ~= 15.70; perspective U ~= 5.69.
    w,_,rgb = images['vertex']
    at = ((60*4+2)*w + 60*4+2)*3
    assert rgb[at] == 57  # 5-bit red 7 expanded to 8 bits.
    assert images['perspective'][2][at] == 16  # red 2, from U=5.


def test_perspective_keeps_gouraud_colors_affine(vulkan_replay, tmp_path):
    stream = tmp_path / 'color.gpu'
    projection_stream(stream, gouraud=True)
    for mode in ('vertex', 'perspective'):
        replay(vulkan_replay, stream, tmp_path / f'{mode}.png', mode)
    assert (tmp_path / 'vertex.png').read_bytes() == (tmp_path / 'perspective.png').read_bytes()


def test_missing_quad_vertex_preserves_other_endpoints(vulkan_replay, tmp_path):
    stream = tmp_path / 'quad.gpu'
    projection_stream(stream, missing_fourth=True)
    report = replay(vulkan_replay, stream, tmp_path / 'on.png', 'perspective')
    replay(vulkan_replay, stream, tmp_path / 'off.png', 'off')
    assert report['pgxp']['precise'] == 0 and report['pgxp']['partial'] == 1
    assert (tmp_path / 'on.png').read_bytes() != (tmp_path / 'off.png').read_bytes()


@pytest.mark.parametrize('payload', [b'', bytes(19)])
def test_truncated_projection_payload_is_rejected(vulkan_replay, tmp_path, payload):
    stream = tmp_path / 'truncated.gpu'
    stream.write_bytes(b'FF8GPUR2' + struct.pack('<II', 7, 0) + payload)
    run = subprocess.run([str(vulkan_replay), str(stream), '1'],
                         env=dict(os.environ, VK_DRIVER_FILES=str(ICD)), capture_output=True, timeout=60)
    assert run.returncode == 2


@pytest.mark.parametrize('origin', [0, 1])
def test_recording_binds_before_projection_and_seeds_late_snapshots(vulkan_replay, tmp_path, origin):
    binary = tmp_path / 'record'
    symbols = ['gpu_gp0', 'gpu_gp0_words', 'gpu_gp1', 'gpu_vblank', 'gpu_read', 'gpu_status', 'pgxp_bind']
    subprocess.run(['cc', '-std=c99', '-O2', '-Wall', '-Wextra', '-Wpedantic', '-Werror',
                    '-DFF8_PGXP', '-Iruntime', '-Ibackends/host', 'tests/pgxp_record_harness.c',
                    'runtime/pgxp.c', 'runtime/gpu_raster_soft.c', 'backends/host/gpu_record.c',
                    *[f'-Wl,--wrap={name}' for name in symbols], '-o', str(binary)], cwd=ROOT, check=True)
    stream = tmp_path / 'capture.gpu'
    subprocess.run([str(binary)], env=dict(os.environ, FF8_GPU_STREAM=str(stream), FF8_GPU_FROM=str(origin)), check=True)
    assert stream.read_bytes().startswith(b'FF8GPUR2')
    report = replay(vulkan_replay, stream, tmp_path / 'capture.png', 'perspective')
    assert report['pgxp']['precise'] == report['pgxp']['candidates'] == 1
    assert report['statuses'] == 1
    strict=replay(vulkan_replay,stream,tmp_path/'strict.png','vertex',policy='strict')
    assert strict['pgxp']['precise']==0 and strict['pgxp']['partial']==1


def test_pgxp_host_initializes_lazy_hardware_and_keeps_guest_state(vulkan_replay, tmp_path):
    """Drive real main/cpu/SDL initialization with an original two-instruction EXE."""
    from recomp.discover import Image, discover
    from recomp.emit import emit
    header = bytearray(2048)
    header[:8] = b'PS-X EXE'
    struct.pack_into('<I', header, 0x10, 0x80010000)
    struct.pack_into('<II', header, 0x18, 0x80010000, 2048)
    text = struct.pack('<II', 0x08004000, 0) + bytes(2040)  # j entry; nop
    exe = tmp_path / 'synthetic.exe'
    exe.write_bytes(header + text)
    generated = vulkan_replay.parent.parent / 'gen'
    image = Image.read(exe)
    emit(image, discover(image), generated, mode='fast', host_mode='precise')
    subprocess.run(['cmake', '--build', str(vulkan_replay.parent), '--target', 'ff8', '-j4'],
                   check=True, capture_output=True, timeout=180)
    states = []
    for mode in ('default', 'off', 'vertex', 'perspective'):
        state = tmp_path / f'{mode}.state'
        command = [str(vulkan_replay.with_name('ff8')), str(exe), '--config', os.devnull, '--headless', '--mute', '--fast',
                   '--stall', '0', '--max-instructions', '0', '--max-vblanks', '2', '--dump-state', str(state)]
        if mode != 'default':
            command += ['--renderer', 'vulkan', '--pgxp', mode]
        run = subprocess.run(command, env=dict(os.environ, VK_DRIVER_FILES=str(ICD),
                             SDL_VIDEODRIVER='offscreen', SDL_AUDIODRIVER='dummy'),
                             text=True, capture_output=True, timeout=30)
        assert run.returncode == 0, run.stdout + run.stderr
        assert 'Stop: VBlank limit reached' in run.stdout and 'VBlanks=2 ' in run.stdout
        if mode != 'default':
            assert 'Vulkan:' in run.stderr and 'falling back' not in run.stderr
        states.append(state.read_bytes())
    assert len(set(states)) == 1


def shared_edge_stream(path):
    data=bytearray(b'FF8GPUR2')
    def row(k,v): data.extend(struct.pack('<II',k,v))
    def gp0(*words):
        for word in words: row(0,word)
    row(1,0x03000000)
    row(1,0x06000000|512|(3712<<12))
    gp0(0xE3000000,0xE407FFFF,0xE1000400)
    for x,y in [(180,20),(20,180),(180,180)]:
        row(7,xy(x,y));data.extend(struct.pack('<qqI',x*65536+49152,y*65536+32768,100))
    # Lower/right triangle has three hits. Upper/left lacks only its unique A.
    gp0(0x200000FF,xy(180,20),xy(20,180),xy(180,180))
    gp0(0x200000FF,xy(20,20),xy(180,20),xy(20,180))
    row(2,0);path.write_bytes(data)


def test_adjacent_polygons_with_partial_provenance_have_no_black_crack(vulkan_replay,tmp_path):
    from tools.render_quality import png,compare
    stream=tmp_path/'shared.gpu';shared_edge_stream(stream)
    replay(vulkan_replay,stream,tmp_path/'off.png','off')
    replay(vulkan_replay,stream,tmp_path/'on.png','vertex')
    result=compare(png(tmp_path/'off.png'),png(tmp_path/'on.png'),'battle')
    assert result['interior_pixels']==0



def test_crack_detector_catches_interior_line_without_counting_silhouette_motion():
    import numpy as np
    from tools.render_quality import compare
    off=np.full((864,1280,3),80,dtype=np.uint8)
    on=off.copy();on[500,100:300]=0
    assert compare(off,on,'battle')['interior_pixels']==200
    off[500,100:300]=0;on=off.copy();on[501,100:300]=0
    result=compare(off,on,'battle')
    assert result['raw_pixels']==200 and result['interior_pixels']==0
