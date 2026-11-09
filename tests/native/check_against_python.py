#!/usr/bin/env python3
"""Prove the native test inputs and oracles equal the Python ones, while Python is here.

Run after building tests/native:   python tests/native/check_against_python.py

Each native case records its inputs and its oracle's answers (lh::equiv::record).
This script re-runs the real pytest bodies with the runtime replaced by a replay of
those records: the replay checks the inputs the pytest body generated are the ones
the native case used, and hands back the native oracle's answer, so the pytest
body's own `assert` compares the Python oracle against the native one on every
vector. Delete this file with Python, together with tests/native/support/equiv.h
and the lh::equiv::record calls; nothing else depends on them.
"""
import argparse
import ctypes
import hashlib
import importlib
import os
from pathlib import Path
import random
import re
import shutil
import struct
import subprocess
import sys
import tempfile
from types import SimpleNamespace
from unittest import mock

ROOT = Path(__file__).resolve().parents[2]
sys.path[:0] = [str(ROOT), str(ROOT / 'tests')]

FILES = ['test_spu_math', 'test_cycle_math', 'test_mdec_math', 'test_media',
         'test_media_play_grants', 'test_mdec_calendar', 'test_gpu', 'test_movie_frames', 'test_pgxp', 'test_host_crt', 'test_host_osd']


class Records:
    def __init__(self, path):
        self.path = path
        self.data = path.read_bytes()
        self.at = 0

    def next(self, fmt):
        values = struct.unpack_from(fmt, self.data, self.at)
        self.at += struct.calcsize(fmt)
        return values

    def bytes(self, count):
        out = self.data[self.at:self.at + count]
        assert len(out) == count, f'{self.path}: expected {count} more bytes'
        self.at += count
        return out

    def done(self):
        assert self.at == len(self.data), f'{self.path}: {len(self.data) - self.at} recorded bytes were never compared'


def records(directory, case):
    path = directory / (re.sub(r'[^0-9A-Za-z]', '_', case) + '.bin')
    assert path.exists(), f'the native case {case} recorded nothing at {path}'
    return Records(path)


class Replay:
    """A ctypes-style function that checks arguments and answers from the records."""

    def __init__(self, stream, fmt, count):
        self.stream, self.fmt, self.count = stream, fmt, count

    def __call__(self, *args):
        row = self.stream.next(self.fmt)
        assert tuple(args) == row[:self.count], (args, row)
        return row[self.count]


def no_compile(*args, **kwargs):
    return SimpleNamespace(stdout=b'', returncode=0)


def check_spu_math(directory, tmp):
    module = importlib.import_module('test_spu_math')
    stream = records(directory, 'test_spu_math::test_adpcm_matches_integer_reference')

    def decoder(data, history, out):
        row = stream.next('<16B2h28h2h')
        assert bytes(data) == bytes(row[:16]), row
        assert list(history) == list(row[16:18]), row
        for i, value in enumerate(row[18:46]):
            out[i] = value
        history[0], history[1] = row[46], row[47]

    module.test_adpcm_matches_integer_reference(decoder)
    stream.done()

    stream = records(directory, 'test_spu_math::test_pcm_multiply_matches_wide_reference')
    library = SimpleNamespace(coefficient=Replay(stream, '<iii', 2), pcm=Replay(stream, '<iii', 2))
    with mock.patch.object(subprocess, 'run', no_compile), mock.patch.object(ctypes, 'CDLL', lambda path: library):
        module.test_pcm_multiply_matches_wide_reference(tmp)
    stream.done()


def check_cycle_math(directory, tmp):
    module = importlib.import_module('test_cycle_math')
    stream = records(directory, 'test_cycle_math::test_cycle_reciprocals')
    library = SimpleNamespace(ratio=Replay(stream, '<QIQ', 2), product=Replay(stream, '<QQQ', 2))
    with mock.patch.object(subprocess, 'run', no_compile), mock.patch.object(ctypes, 'CDLL', lambda path: library):
        module.test_cycle_reciprocals(tmp)
    stream.done()


def check_mdec_math(directory, tmp):
    module = importlib.import_module('test_mdec_math')
    stream = records(directory, 'test_mdec_math::test_idct_rounding_and_full_int16_bounds')

    def transform(values, scale):
        row = stream.next('<192h')
        assert list(values) == list(row[:64]) and list(scale) == list(row[64:128]), row[:128]
        for i in range(64):
            values[i] = row[128 + i]

    module.test_idct_rounding_and_full_int16_bounds(transform)
    stream.done()


def media_run(stream, expected_input, output):
    def run(command, input=None, stdout=None, check=None, timeout=None):
        assert input == expected_input(), command
        return SimpleNamespace(stdout=output(), returncode=0)
    return run


def check_media(directory, tmp):
    module = importlib.import_module('test_media')
    stream = records(directory, 'test_media::test_idct_fixed_point')

    def run(command, input=None, **kwargs):
        row = stream.next('<192h')
        assert input == struct.pack('<128h', *row[:128]), command
        return SimpleNamespace(stdout=struct.pack('<64h', *row[128:]), returncode=0)

    with mock.patch.object(subprocess, 'run', run):
        module.test_idct_fixed_point(Path('/nonexistent'))
    stream.done()

    for bits, stereo in [(4, False), (4, True), (8, False), (8, True)]:
        stream = records(directory, f'test_media::test_xa_predictor_arithmetic_and_history[{bits}-{stereo}]')
        raw = stream.bytes(2352)
        pcm = stream.data[2352:]

        def run_xa(command, input=None, **kwargs):
            assert input == raw and command[1:] == ['xa'], command
            return SimpleNamespace(stdout=pcm, returncode=0)

        with mock.patch.object(subprocess, 'run', run_xa):
            module.test_xa_predictor_arithmetic_and_history(Path('/nonexistent'), bits, stereo)
        stream.at = len(stream.data)
        stream.done()


def check_mdec_calendar(directory, tmp):
    module = importlib.import_module('test_mdec_calendar')
    for depth in range(4):
        for layout in range(4):
            for control in range(2):
                stream = records(directory, f'test_mdec_calendar::test_word_calendar_observations[{control}-{layout}-{depth}]')
                remembered = {}

                class Model:
                    def __init__(self, index):
                        self.index = index

                    def __getattr__(self, name):
                        return lambda *args: 0

                    def calendar_create(self, *args):
                        assert args == (depth, layout, control)
                        return 0

                    def calendar_spans(self, cpu):
                        return 1

                    def calendar_advance(self, cpu, cycle):
                        if self.index == 0:
                            remembered['cycle'] = stream.next('<Q')[0]
                        assert cycle == remembered['cycle'], (depth, layout, control, cycle)

                module.test_word_calendar_observations([Model(0), Model(1)], depth, layout, control)
                stream.done()


def check_gpu(directory, tmp):
    module = importlib.import_module('test_gpu')
    stream = records(directory, 'test_gpu::test_truncated_packets_and_random_control_streams')

    class Gpu:
        g = 0

        def __init__(self):
            self.pending = None
            self.lib = SimpleNamespace(gpu_read=lambda g: 0)

        def gp0(self, a, b):
            row = stream.next('<III')
            assert (a, b) == row[:2], row
            self.pending = row[2]

        def gp1(self, word):
            if word != 0x01000000:
                assert word == self.pending, (word, self.pending)

        def status(self):
            return 1 << 26

    module.test_truncated_packets_and_random_control_streams(Gpu())
    stream.done()

    png_cases = [f'test_gpu::test_present_pngs_and_vram_exit[{headless}]' for headless in ('True', 'False')]
    png_cases.append('test_gpu::test_zero_display_ranges_are_black_one_pixel_png')
    for case in png_cases:
        stream = records(directory, case)
        count = 0
        while stream.at < len(stream.data):
            size = stream.next('<I')[0]
            png = stream.bytes(size)
            width, height, rgb_size = stream.next('<III')
            rgb = stream.bytes(rgb_size)
            path = tmp / f'png-{count}.png'
            path.write_bytes(png)
            assert module.read_png(path) == (width, height, rgb), case
            count += 1
        assert count, f'{case}: no PNGs recorded'


def check_instrumented_hw(build):
    from tools.mdec_trace import instrument_hw
    generated = (build / 'generated/test_mdec_calendar/hw.c').read_bytes()
    assert generated == instrument_hw((ROOT / 'runtime/hw.c').read_text()).encode(), 'generated hw.c differs'


def check_py_random(directory):
    stream = records(directory, 'oracle_self_tests::py_random_vectors_for_the_python_check')
    for seed in [0, 1, 8, 90210, 0xadc0, 0x1820, 0x505358, 1400, 0xffffffff, 0x100000000,
                 0x100000007, 0xfffffffffffffffe]:
        r = random.Random(seed)
        for _ in range(16):
            assert stream.next('<d')[0] == r.random(), ('random', seed)
        for k in range(1, 65):
            assert stream.next('<Q')[0] == r.getrandbits(k), ('getrandbits', seed, k)
        for _ in range(64):
            row = stream.next('<qqqqqq')
            assert row == (r.randrange(1, 2), r.randrange(-98304, 98305), r.randrange(1, 1300),
                           r.randrange(65536), r.randint(-32768, 32767), r.randrange(65537)), ('randrange', seed)
        for size in [1, 3, 4, 5, 14, 16, 31, 32]:
            assert stream.bytes(size) == r.randbytes(size), ('randbytes', seed, size)
    stream.done()


def write_png_vectors(directory):
    import zlib
    vectors = directory / 'png_vectors'
    vectors.mkdir()
    rng = random.Random(0x706e67)
    for name, width, height, payload, level in [
        ('flat-stored', 4, 3, bytes([255, 0, 0]) * 12, 0),
        ('flat-compressed', 4, 3, bytes([255, 0, 0]) * 12, 9),
        ('pattern', 16, 16, bytes(i * 37 & 255 for i in range(16 * 16 * 3)), 6),
        ('random-stored', 32, 8, rng.randbytes(32 * 8 * 3), 0),
        ('random-compressed', 32, 8, rng.randbytes(32 * 8 * 3), 1),
        ('first-up', 4, 3, bytes(range(36)), 0),
        ('random-up', 32, 8, rng.randbytes(32 * 8 * 3), 6),
    ]:
        raw = b''.join(b'\0' + payload[row * width * 3:(row + 1) * width * 3] for row in range(height))
        if name.endswith('-up'):
            rows = []
            for y in range(height):
                row = payload[y * width * 3:(y + 1) * width * 3]
                previous = payload[(y - 1) * width * 3:y * width * 3] if y else bytes(width * 3)
                use_up = name == 'first-up' or y % 2
                rows.append(b'\2' + bytes((a - b) % 256 for a, b in zip(row, previous)) if use_up else b'\0' + row)
            raw = b''.join(rows)
        compressed = zlib.compress(raw, level)
        def chunk(kind, body):
            return struct.pack('>I', len(body)) + kind + body + struct.pack('>I', zlib.crc32(kind + body))
        png = b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', width, height, 8, 2, 0, 0, 0))
        png += chunk(b'IDAT', compressed) + chunk(b'IEND', b'')
        (vectors / f'{name}.png').write_bytes(png)
        (vectors / f'{name}.expected').write_text(f'{width} {height} {hashlib.sha256(payload).hexdigest()}')
    return vectors



def check_numpy_random(directory):
    import numpy as np
    stream = records(directory, 'rendering_oracle_checks::numpy_uint8_vectors_for_the_python_check')
    for seed in (0, 1, 0x15, 0x1820, 0xffffffff, 0x100000007):
        random = np.random.default_rng(seed)
        for high in (9, 248, 256):
            for size in (1, 3, 4, 7, 1024):
                low = 8 if high == 9 else 0
                assert stream.bytes(size) == random.integers(low, high, size=size, dtype=np.uint8).tobytes(), (seed, low, high, size)
    stream.done()


def check_image_numeric(directory):
    import numpy as np
    module = importlib.import_module('test_host_crt')
    quality = importlib.import_module('tools.render_quality')
    stream = records(directory, 'rendering_oracle_checks::image_numeric_vectors_for_the_python_check')
    for width in (7, 320, 640):
        image = np.array([((x * 7 + y * 3 + c * 51) & 255)
                          for y in range(224) for x in range(width) for c in range(3)], dtype=np.uint8).reshape(224, width, 3)
        data = image.tobytes()
        assert stream.bytes(len(data)) == data
        mean, = stream.next('<d')
        rect = (1, 2, width - 2, 220)
        assert abs(mean - module.region_mean_linear(data, (width, 224), rect)) < 1e-11
        profile = np.array(stream.next('<220d'))
        np.testing.assert_array_equal(profile, module.row_profile(data, (width, 224), rect))
        cycles, = stream.next('<i')
        assert cycles == module.dominant_cycles(profile)
        length, = stream.next('<Q')
        path = directory / 'native-vector.png'
        path.write_bytes(stream.bytes(length))
        np.testing.assert_array_equal(quality.png(path), image)
    for scene in ('battle', 'world', 'dorm'):
        off = np.full((864, 1280, 3), 80, dtype=np.uint8)
        on = off.copy(); on[500, 100:300] = 0
        for iteration in range(2):
            expected = quality.compare(off, on, scene)
            assert stream.next('<3q') == tuple(expected[key] for key in ('raw_pixels', 'interior_pixels', 'roi_pixels'))
            off = on.copy(); on[501, 100:300] = 0
    stream.done()



def check_movie_frames(directory, tmp):
    import json
    import numpy as np
    module = importlib.import_module('test_movie_frames')
    real_rng, real_compare, real_pack = np.random.default_rng, module.compare_frames, module.pack_rgb15
    for name in ('test_movie_band_is_not_hidden_by_whole_frame_average',
                 'test_movie_switch_to_rgb15_does_not_hide_a_misplaced_band'):
        stream = records(directory, 'test_movie_frames::' + name)
        class Random:
            def __init__(self, seed):
                self.random = real_rng(seed)
            def integers(self, *args, **kwargs):
                data = self.random.integers(*args, **kwargs)
                assert stream.bytes(data.size) == data.tobytes(), 'NumPy movie input differs'
                return data
        def compare(*args, **kwargs):
            expected = real_compare(*args, **kwargs)
            size, = stream.next('<Q')
            actual = json.loads(stream.bytes(size))
            for report in (actual, expected):
                for failure in report['failures']:
                    failure['png'] = Path(failure['png']).name
            assert actual.keys() == expected.keys()
            for key in expected:
                if isinstance(expected[key], float):
                    assert abs(actual[key] - expected[key]) < 1e-11, (key, actual[key], expected[key])
                else:
                    assert actual[key] == expected[key], (key, actual[key], expected[key])
            return actual
        def pack(data):
            expected = real_pack(data)
            actual = np.frombuffer(stream.bytes(expected.size), dtype=np.uint8).reshape(expected.shape)
            np.testing.assert_array_equal(actual, expected)
            return actual.copy()
        folder = tmp / name
        folder.mkdir()
        with mock.patch.object(np.random, 'default_rng', Random), mock.patch.object(module, 'compare_frames', compare), mock.patch.object(module, 'pack_rgb15', pack):
            getattr(module, name)(folder)
        stream.done()
    stream = records(directory, 'test_movie_frames::test_movie_checker_accepts_integer_color_rounding')
    real_tile = module.tile_errors
    def tile(a, b):
        expected = real_tile(a, b)
        actual = np.array(stream.next('<280d')).reshape(14, 20)
        np.testing.assert_array_equal(actual, expected)
        return actual
    with mock.patch.object(module, 'tile_errors', tile):
        module.test_movie_checker_accepts_integer_color_rounding()
    stream.done()
    for left, right, lag, expected, identity in [(.84,.93,0,True,'0.84-0.93-0.0-True'),
            (.99,.99,.1,False,'0.99-0.99-0.1-False'),(.70,.80,0,False,'0.7-0.8-0.0-False'),
            (float('nan'),.99,0,False,'nan-0.99-0.0-False')]:
        stream = records(directory, 'test_movie_frames::test_audio_anchor_requires_a_strong_match_and_stereo_agreement[' + identity + ']')
        answer, = stream.next('<B')
        real_anchor = module.matched_anchor
        def anchor(channels):
            assert bool(answer) == real_anchor(channels)
            return bool(answer)
        with mock.patch.object(module, 'matched_anchor', anchor):
            module.test_audio_anchor_requires_a_strong_match_and_stereo_agreement(left,right,lag,expected)
        stream.done()


def check_crt_numeric(directory, tmp):
    import numpy as np
    module = importlib.import_module('test_host_crt')
    real_profile, real_mean = module.row_profile, module.region_mean_linear
    for index, size in enumerate(((1920,1080),(1280,960),(3840,2160))):
        for preset in module.PRESETS:
            stream = records(directory, f'test_host_crt::test_preset_scanlines_borders_brightness_and_determinism[size{index}-{preset}]')
            width,height,x,y,w,h = stream.next('<6i')
            out = stream.bytes(width*height*3)
            profile = np.array(stream.next(f'<{h}d'))
            cycles, = stream.next('<i')
            centre = stream.next('<4i')
            reference = stream.bytes(width*height*3)
            mean,refmean = stream.next('<2d')
            frame = (0,0,1280,960) if size==(1280,960) else (240,0,1440,1080) if size==(1920,1080) else (480,0,2880,2160)
            def present(tool, rgb, actual_size, actual_preset, out_dir):
                assert actual_size==size and rgb==bytes([128])*(320*240*3)
                return reference if actual_preset=='off' else out
            def row(data, actual_size, rect):
                assert rect==(x,y,w,h)
                np.testing.assert_array_equal(profile, real_profile(data,actual_size,rect))
                assert cycles==module.dominant_cycles(profile)
                return profile
            def linear(data, actual_size, rect):
                assert rect==centre
                value = refmean if data==reference else mean
                assert abs(value-real_mean(data,actual_size,rect))<1e-11
                return value
            with mock.patch.object(module,'present',present),mock.patch.object(module,'row_profile',row),mock.patch.object(module,'region_mean_linear',linear):
                module.test_preset_scanlines_borders_brightness_and_determinism(None,lambda *args:(frame,frame),tmp,preset,size)
            stream.done()
    stream=records(directory,'test_host_crt::test_interlaced_scanout_uses_thinner_beams')
    width,height,x,y,w,h=stream.next('<6i');out=stream.bytes(width*height*3)
    profile=np.array(stream.next(f'<{h}d'));cycles,=stream.next('<i')
    np.testing.assert_array_equal(profile,real_profile(out,(width,height),(x,y,w,h)))
    assert cycles==module.dominant_cycles(profile)
    with mock.patch.object(module,'present',lambda *a,**k:out):
        module.test_interlaced_scanout_uses_thinner_beams(None,lambda *a:((240,0,1440,1080),(240,0,1440,1080)),tmp)
    stream.done()


def check_osd_numeric(directory, tmp):
    import numpy as np
    module=importlib.import_module('test_host_osd')
    for index, (size, age) in enumerate([((1280,720),120),((1920,1080),60),((1920,1080),120),((1920,1080),2070),((2560,1440),120),((3840,2160),120)]):
        stream=records(directory,f'test_host_osd::test_soft_vulkan_composition[size{index}-{age}]')
        width,height=stream.next('<2i');assert (width,height)==size
        a,b=stream.bytes(width*height*3),stream.bytes(width*height*3)
        difference,=stream.next('<i')
        assert difference==np.abs(np.frombuffer(a,np.uint8).astype(int)-np.frombuffer(b,np.uint8).astype(int)).max()
        def run(command,**kwargs):
            output=Path(command[command.index('--out')+1]) if '--out' in command else Path(command[3])
            output.write_bytes(a if '--out' in command else b)
            return SimpleNamespace(returncode=0)
        folder=tmp/f'osd-{index}';folder.mkdir()
        with mock.patch.object(module.subprocess,'run',run):
            module.test_soft_vulkan_composition(Path('preview'),Path('soft'),folder,size,age)
        stream.done()


def check_pgxp_quality(directory, tmp):
    import numpy as np
    module=importlib.import_module('test_pgxp')
    quality=importlib.import_module('tools.render_quality')
    stream=records(directory,'test_pgxp::test_adjacent_polygons_with_partial_provenance_have_no_black_crack')
    width,height=stream.next('<2I')
    off=np.frombuffer(stream.bytes(width*height*3),np.uint8).reshape(height,width,3)
    on=np.frombuffer(stream.bytes(width*height*3),np.uint8).reshape(height,width,3)
    raw,interior,roi=stream.next('<3q')
    expected=quality.compare(off,on,'battle')
    assert expected==dict(raw_pixels=raw,interior_pixels=interior,roi_pixels=roi)
    with mock.patch.object(module,'shared_edge_stream',lambda path:None),mock.patch.object(module,'replay',lambda *args:None),mock.patch.object(quality,'png',lambda path:off if path.name=='off.png' else on):
        module.test_adjacent_polygons_with_partial_provenance_have_no_black_crack(None,tmp)
    module.test_crack_detector_catches_interior_line_without_counting_silhouette_motion()
    stream.done()



def check_pause_probe(directory, tmp):
    module=importlib.import_module('test_gui_pause_player')
    stream=records(directory,'player_probe_checks::pause_probe_replays_the_player_compile_and_link_commands')
    for name in ('pause-command','pause-arguments'):
        length,=stream.next('<Q');native=Path(stream.bytes(length).decode())
        folder=tmp/name;folder.mkdir()
        factory=SimpleNamespace(mktemp=lambda prefix:folder)
        rebuilt=module.pause_player.__wrapped__(native.parent.parent/'lionheart',factory)
        assert native.read_bytes()==rebuilt.read_bytes(), 'pause fixture relink differs'
    stream.done()


def check_pgxp_generator(directory, tmp):
    from recomp.discover import Image,discover
    from recomp.emit import emit
    stream=records(directory,'test_pgxp::test_pgxp_host_initializes_lazy_hardware_and_keeps_guest_state')
    header=bytearray(2048);header[:8]=b'PS-X EXE'
    struct.pack_into('<I',header,0x10,0x80010000);struct.pack_into('<II',header,0x18,0x80010000,2048)
    exe=tmp/'projection.exe';exe.write_bytes(header+struct.pack('<II',0x08004000,0)+bytes(2040))
    image=Image.read(exe);output=tmp/'projection-gen';emit(image,discover(image),output,mode='fast',host_mode='precise')
    count,=stream.next('<I');native={}
    for _ in range(count):
        length,=stream.next('<Q');name=stream.bytes(length).decode();length,=stream.next('<Q');native[name]=stream.bytes(length)
    expected={str(path.relative_to(output)):path.read_bytes() for path in output.rglob('*') if path.is_file()}
    assert native==expected, 'synthetic PGXP generator output differs'
    stream.done()



def check_render_quality_cli(build, tmp):
    import numpy as np
    import zlib
    root = tmp / 'render-quality'
    root.mkdir()
    def png(path,pixels):
        def chunk(tag,data):return struct.pack('>I',len(data))+tag+data+struct.pack('>I',zlib.crc32(tag+data))
        h,w,_=pixels.shape
        rows=b''.join(b'\0'+row.tobytes() for row in pixels)
        path.write_bytes(b'\x89PNG\r\n\x1a\n'+chunk(b'IHDR',struct.pack('>IIBBBBB',w,h,8,2,0,0,0))+chunk(b'IDAT',zlib.compress(rows))+chunk(b'IEND',b''))
    off, on = root / 'off', root / 'on'
    off.mkdir(); on.mkdir()
    a = np.full((864, 1280, 3), 80, np.uint8)
    b = a.copy(); b[500, 100:300] = 0
    for number in range(11, 21):
        png(off / f'frame-{number:06d}.png', a)
        png(on / f'frame-{number:06d}.png', b)
    for scene in ('battle', 'world', 'dorm'):
        for budget in (8, 200):
            output = root / 'result.json'
            args = ['--off', str(off), '--on', str(on), '--scene', scene, '--output', str(output), '--max-crack-pixels', str(budget)]
            expected = subprocess.run([sys.executable, ROOT / 'tools/render_quality.py', *args], capture_output=True)
            report = output.read_bytes()
            actual = subprocess.run([build / 'harness/test_pgxp/quality', *args], capture_output=True)
            assert (actual.returncode, actual.stdout) == (expected.returncode, expected.stdout)
            assert output.read_bytes() == report, (scene, budget)



def check_device_selection(build, tmp):
    import conftest
    root = ROOT
    binary = build / 'fixtures/vulkan/host/gpu_replay'
    real_run=subprocess.run
    for name,variable,value in [('modern','VK_DRIVER_FILES',str(tmp/'no-such-icd.json')),('legacy','VK_ICD_FILENAMES',str(tmp/'no-such-icd.json')),('automatic','VK_LOADER_DRIVERS_SELECT','no-such-driver*.json')]:
        environment={k:v for k,v in os.environ.items() if k not in ('VK_DRIVER_FILES','VK_ICD_FILENAMES','VK_LOADER_DRIVERS_SELECT')}
        environment[variable]=value
        folder=tmp/('device-'+name);folder.mkdir(exist_ok=True)
        (folder/'host').mkdir(exist_ok=True)
        target=folder/'host/gpu_replay'
        if not target.exists():target.symlink_to(binary)
        def run(command,**kwargs):
            if str(command[0])=='cmake':return SimpleNamespace(returncode=0,stdout=b'',stderr=b'')
            return real_run(command,**kwargs)
        factory=SimpleNamespace(mktemp=lambda prefix:folder)
        with mock.patch.dict(os.environ,environment,clear=True),mock.patch.object(conftest.subprocess,'run',run):
            try:
                conftest.vulkan_replay.__wrapped__(factory)
            except BaseException as error:
                expected=str(error).strip()
            else:raise AssertionError('device fixture unexpectedly ran')
        native=real_run(['nice','-n','10',str(build/'lionheart_tests'),'--run','oracle_self_tests::vulkan_fixture_preserves_loader_selection'],env=environment,text=True,capture_output=True)
        prefix='No usable Vulkan device: ' if name=='automatic' else 'The explicitly selected Vulkan driver is unavailable:'
        actual=prefix+native.stdout.split(prefix,1)[1]
        assert actual.strip()==expected,(actual,expected)
        assert native.returncode==(77 if name=='automatic' else 1),(name,native.stdout)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('build', nargs='?', type=Path, default=ROOT / 'build/native-tests')
    args = parser.parse_args()
    build = args.build.resolve()
    runner = build / 'lionheart_tests'
    if not runner.exists():
        parser.error(f'build tests/native first: {runner} is missing')
    with tempfile.TemporaryDirectory(prefix='lh-equiv-') as tmp_name:
        tmp = Path(tmp_name)
        directory = tmp / 'records'
        directory.mkdir()
        write_png_vectors(directory)
        cases = [line.split('\t')[1] for line in subprocess.run([runner, '--list'], check=True, text=True,
                                                                stdout=subprocess.PIPE).stdout.splitlines()]
        wanted = [c for c in cases if c.split('::')[0] in FILES or c.startswith(('oracle_self_tests::', 'rendering_oracle_checks::', 'player_probe_checks::'))]
        environment = dict(os.environ, LH_EQUIV_DIR=str(directory))
        failed = [c for c in wanted if subprocess.run([runner, '--build-info', build / 'lh_build_info.txt', '--run', c],
                                                      env=environment, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                                      text=True, timeout=600).returncode != 0]
        assert not failed, f'native cases failed: {failed}'
        checks = [('Vulkan: both driver overrides and automatic skip reasons equal pytest', lambda: check_device_selection(build, tmp)),
                  ('render-quality CLI: stdout, JSON bytes and exit status', lambda: check_render_quality_cli(build, tmp)),
                  ('pause probe: both original compile receipt formats relink identically', lambda: check_pause_probe(directory, tmp)),
                  ('PGXP: synthetic generator bytes equal Python emit', lambda: check_pgxp_generator(directory, tmp)),
                  ('movie frames: full inputs, tile reports, RGB15 and anchors', lambda: check_movie_frames(directory, tmp)),
                  ('CRT: real pytest bodies and native numeric answers', lambda: check_crt_numeric(directory, tmp)),
                  ('OSD: real pytest composition bodies and numeric tolerance', lambda: check_osd_numeric(directory, tmp)),
                  ('PGXP: real pytest crack checks and numeric counts', lambda: check_pgxp_quality(directory, tmp)),
                  ('NumPy: uint8 fills for every seed, bound and length', lambda: check_numpy_random(directory)),
                  ('numeric images: linear means, profiles, DFT peaks and crack counts', lambda: check_image_numeric(directory)),
                  ('generator: every method and seed against CPython', lambda: check_py_random(directory)),
                  ('instrumented hw.c is what tools/mdec_trace.py generates', lambda: check_instrumented_hw(build)),
                  ('test_spu_math: inputs and oracles', lambda: check_spu_math(directory, tmp)),
                  ('test_cycle_math: inputs and oracles', lambda: check_cycle_math(directory, tmp)),
                  ('test_mdec_math: inputs and oracle', lambda: check_mdec_math(directory, tmp)),
                  ('test_media: IDCT and XA inputs and oracles', lambda: check_media(directory, tmp)),
                  ('test_mdec_calendar: all random step sequences', lambda: check_mdec_calendar(directory, tmp)),
                  ('test_gpu: control stream and PNG oracle', lambda: check_gpu(directory, tmp))]
        for label, check in checks:
            check()
            print(f'equal  {label}')
    print('native inputs and oracles equal the Python ones')


if __name__ == '__main__':
    main()
