"""Byte parity between the C generator and the Python recompiler, and its error handling."""
import hashlib
import json
import os
from pathlib import Path
import random
import shutil
import struct
import subprocess
from types import SimpleNamespace

import pytest

from recomp.discover import Image, discover
from recomp.emit import emit
from recomp.locals import cache_registers, lower, qualify, read_selection
from test_recompiler import BASE, CASES, PCS, RET, imm, j, r, table, table_pc

ROOT = Path(__file__).resolve().parents[1]
SANITIZE = bool(os.environ.get('FF8_TEST_SANITIZE'))


@pytest.fixture(scope='session')
def tools():
    build = ROOT / ('build/generator-test-sanitize' if SANITIZE else 'build/generator-test')
    configure = ['cmake', '-S', str(ROOT / 'generator'), '-B', str(build), '-DCMAKE_BUILD_TYPE=Release']
    if SANITIZE:
        configure.append('-DLIONHEART_GENERATOR_SANITIZE=ON')
    subprocess.run(['nice', '-n', '10', *configure], check=True, capture_output=True)
    subprocess.run(['nice', '-n', '10', 'cmake', '--build', str(build), '-j', '4'], check=True, capture_output=True)
    return build / 'lionheart-gen-spec', build / 'lionheart-gen'


def image_spec(data, base=BASE, entry=None, **extra):
    return {'data': bytes(data).hex(), 'base': base, 'entry': base if entry is None else entry, **extra}


def build_image(row):
    ranges = tuple(tuple(pair) for pair in row.get('code_ranges', ()))
    image = Image(bytes.fromhex(row['data']), row['base'], row['entry'],
                  code_ranges=ranges, entry_points=tuple(row.get('entry_points', ())),
                  timing_entry_points=tuple(row.get('timing_entry_points', ())))
    if 'metadata' in row:
        image.metadata = row['metadata']
    return image


def python_emit(spec, output):
    main = spec['main']
    image = build_image(main)
    seeds = [*main.get('seeds', ()), *(image.timing_entry_points if spec.get('timing', 'exact') != 'exact' else ())]
    functions = discover(image, seeds, main.get('scan_calls', True))
    overlays = []
    for row in spec.get('overlays', ()):
        overlay = build_image(row)
        overlays.append((row['index'], overlay, discover(overlay, row.get('seeds', ()), row.get('scan_calls', True))))
    emit(image, functions, output, mode=spec.get('mode', 'fast'), overlays=overlays,
         host_mode=spec.get('host_mode', 'precise'), timing=spec.get('timing', 'exact'),
         register_cache=spec.get('register_cache'))


def tree(path):
    return {p.relative_to(path).as_posix(): p.read_bytes() for p in sorted(path.rglob('*')) if p.is_file()}


def assert_parity(tools, spec, tmp_path, name):
    tool, _ = tools
    spec_path = tmp_path / f'{name}.json'
    spec_path.write_text(json.dumps(spec))
    native = tmp_path / f'{name}-c'
    reference = tmp_path / f'{name}-py'
    subprocess.run([str(tool), str(spec_path), str(native)], check=True, timeout=120, capture_output=True, text=True)
    python_emit(spec, reference)
    expected = tree(reference)
    actual = tree(native)
    assert expected.keys() == actual.keys()
    for key in expected:
        if actual[key] != expected[key]:
            mismatch = next((i for i, (a, b) in enumerate(zip(actual[key], expected[key])) if a != b),
                            min(len(actual[key]), len(expected[key])))
            pytest.fail(f'{name}: {key} differs at byte {mismatch}')


@pytest.mark.parametrize('timing', ['exact', 'block', 'loose'])
@pytest.mark.parametrize('cache', [False, True])
def test_recompiler_cases(tools, tmp_path, timing, cache):
    data = bytearray(len(CASES) * 0x200)
    for name, words in CASES.items():
        struct.pack_into(f'<{len(words)}I', data, PCS[name] - BASE, *words)
    struct.pack_into('<2I', data, table - BASE, table_pc + 32, table_pc + 44)
    seeds = [pc for name, pc in PCS.items() if name != 'fallback_entry']
    overlays = []
    for index in (0, 1, 32, 64):
        words = [r(33, 31, 0, 25), j(3, PCS['callee']), 0, imm(9, 0, 2, 42 + index % 32), r(33, 25, 0, 31), *RET]
        blob = struct.pack(f'<{len(words)}I', *words) + bytes(16)
        overlays.append(image_spec(blob, 0x80098000, index=index))
    for mode, host in (('fast', 'precise'), ('fast', 'fast'), ('trace', 'precise'), ('trace', 'fast')):
        if timing != 'exact' and mode == 'trace':
            continue
        spec = {'mode': mode, 'host_mode': host, 'timing': timing, 'main': image_spec(data, seeds=seeds), 'overlays': overlays}
        if cache:
            spec['register_cache'] = dict(max_registers=8, functions=[f'f_{pc:08x}' for pc in seeds] +
                                          [f'ov{row["index"]}_f_{row["entry"]:08x}' for row in overlays])
        assert_parity(tools, spec, tmp_path, f'tier1-{mode}-{host}')


def random_word(rng, base, count):
    kind = rng.random()
    reg = lambda: rng.randrange(32)
    if kind < 0.45:
        return rng.getrandbits(32)
    if kind < 0.70:
        return imm(rng.choice([4, 5, 6, 7, 1, 2, 3]), reg(), reg(), rng.randrange(-16, 16)) if rng.random() < 0.8 \
            else j(rng.choice([2, 3]), base + rng.randrange(count) * 4)
    if kind < 0.85:
        return r(rng.choice([0, 2, 3, 4, 6, 7, 8, 9, 12, 13, 16, 17, 18, 19, 24, 25, 26, 27, 32, 33, 34, 35, 36, 37, 38, 39, 42, 43]),
                 reg(), reg(), reg(), rng.randrange(32))
    return imm(rng.choice([8, 9, 10, 11, 12, 13, 14, 15, 32, 33, 34, 35, 36, 37, 38, 40, 41, 42, 43, 46, 50, 58, 16, 18, 49]),
               reg(), reg(), rng.getrandbits(16))


@pytest.mark.parametrize('seed', range(24))
@pytest.mark.parametrize('timing', ['exact', 'block', 'loose'])
def test_random_programs(tools, tmp_path, seed, timing):
    rng = random.Random(0x4c48 + seed)
    count = rng.choice([64, 128, 256, 600])
    base = BASE + rng.choice([0, 4, 8])
    words = [random_word(rng, base, count) for _ in range(count)]
    data = bytearray(struct.pack(f'<{count}I', *words))
    # Sprinkle the zero runs that end discovery, and prologues that seed it.
    if rng.random() < 0.5:
        start = rng.randrange(0, len(data) - 64)
        data[start:start + 64] = bytes(64)
    seeds = [base + rng.randrange(count) * 4 for _ in range(rng.randrange(1, 8))]
    mode, host = rng.choice([('fast', 'precise'), ('fast', 'fast'), ('trace', 'precise'), ('trace', 'fast')])
    if timing != 'exact':
        mode = 'fast'
    overlay_data = bytearray(struct.pack(f'<{count // 2}I', *[random_word(rng, 0x80098000, count // 2) for _ in range(count // 2)]))
    spec = {'mode': mode, 'host_mode': host, 'timing': timing,
            'main': image_spec(data, base, base + rng.randrange(count) * 4, seeds=seeds, scan_calls=rng.random() < 0.5),
            'overlays': [image_spec(overlay_data, 0x80098000, index=rng.choice([0, 1, 27, 33, 70]),
                                    seeds=[0x80098000], scan_calls=False)]}
    if timing != 'exact':
        functions = discover(build_image(spec['main']), seeds, spec['main']['scan_calls'])
        spec['register_cache'] = dict(max_registers=1 + seed % 8,
                                      functions=[f'f_{pc:08x}' for pc in functions] or ['f_00000000'])
    assert_parity(tools, spec, tmp_path, f'random{seed}')


def split_program(functions, base=BASE):
    words = []
    for index in range(functions):
        words += [imm(9, 0, 2, index), *RET]
    return struct.pack(f'<{len(words)}I', *words), [base + index * 12 for index in range(functions)]


@pytest.mark.parametrize('functions', [1, 23, 24, 25, 47, 48, 49, 73])
def test_unit_split_boundaries(tools, tmp_path, functions):
    data, seeds = split_program(functions)
    overlay_data, overlay_seeds = split_program(functions, 0x80098000)
    spec = {'main': image_spec(data, seeds=seeds, scan_calls=False),
            'overlays': [image_spec(overlay_data, 0x80098000, index=5, seeds=overlay_seeds, scan_calls=False)]}
    assert_parity(tools, spec, tmp_path, f'split{functions}')


def test_duplicate_owners_across_sets(tools, tmp_path):
    shared, seeds = split_program(30)
    variant = bytearray(shared)
    variant[-4:] = struct.pack('<I', 0x12345678)
    main = image_spec(shared, seeds=seeds, scan_calls=False)
    spec = {'main': main, 'overlays': [
        image_spec(shared, BASE, index=1, seeds=seeds, scan_calls=False),
        image_spec(shared, BASE, index=2, seeds=seeds, scan_calls=False),
        image_spec(bytes(variant), BASE, index=33, seeds=seeds, scan_calls=False),
        image_spec(shared, BASE + 0x1000, index=34, seeds=[BASE + 0x1000 + (pc - BASE) for pc in seeds], scan_calls=False),
    ]}
    for mode in ('fast', 'trace'):
        spec['mode'] = mode
        assert_parity(tools, spec, tmp_path, f'dupes-{mode}')


def jump_table_program(entries, far_target=False, base=BASE):
    table_address = base + 0x400
    words = [imm(15, 0, 8, (table_address + 0x8000) >> 16), imm(9, 8, 8, table_address & 65535),
             r(0, rt=4, rd=9, sh=2), r(33, 8, 9, 8), imm(35, 8, 8, 0), 0, r(8, 8), 0]
    case = base + len(words) * 4
    data = bytearray(max(0x800, 0x400 + (entries + 8) * 4))
    struct.pack_into(f'<{len(words)}I', data, 0, *words)
    for row in range(entries):
        target = case + (row % 4) * 8
        struct.pack_into('<I', data, 0x400 + row * 4, target if not (far_target and row == entries - 1) else base + 0x20000)
    for row in range(4):
        struct.pack_into('<2I', data, len(words) * 4 + row * 8, *RET)
    return bytes(data)


@pytest.mark.parametrize('entries,far', [(2, False), (3, False), (256, False), (257, False), (1, False), (4, True)])
def test_jump_tables(tools, tmp_path, entries, far):
    spec = {'main': image_spec(jump_table_program(entries, far), seeds=[BASE], scan_calls=False)}
    for mode, host in (('fast', 'precise'), ('fast', 'fast'), ('trace', 'fast')):
        spec['mode'] = mode
        spec['host_mode'] = host
        assert_parity(tools, spec, tmp_path, f'jump-{entries}-{far}-{mode}-{host}')


def idle_program():
    words = [imm(35, 20, 8, 0), 0, imm(9, 8, 8, -1), imm(43, 20, 8, 0), imm(35, 20, 8, 0), 0, imm(5, 8, 9, 3), 0,
             r(8, rs=31), 0, imm(15, 0, 8, 0x8010), imm(35, 8, 8, 4), 0, r(42, 8, 10, 8), imm(5, 8, 0, -15), 0, r(8, rs=31), 0]
    return struct.pack(f'<{len(words)}I', *words)


def poll_program(base, shape):
    getter = base + (0x200c if shape == 'cross' else 0x100)
    address = {'scratch': 0x1f800000, 'mmio': 0x1f801070}.get(shape, 0x80100004)
    words = [r(33, 31, 0, 16), j(3, getter), 0, imm(5, 2, 0, -3), 0, r(8, rs=16), 0]
    words += [0] * ((getter - base) // 4 - len(words))
    words += [imm(15, 0, 2, ((address + 0x8000) >> 16) & 0xffff), imm(35, 2, 2, address & 0xffff), r(8, rs=31),
              imm(43, 20, 9, 0) if shape == 'side_effect' else 0]
    return struct.pack(f'<{len(words)}I', *words)


@pytest.mark.parametrize('shape', ['ram', 'cross', 'scratch', 'mmio', 'side_effect'])
def test_poll_sites(tools, tmp_path, shape):
    base = 0x80010ff4 if shape == 'cross' else 0x80010000
    spec = {'main': image_spec(poll_program(base, shape), base)}
    for host in ('precise', 'fast'):
        spec['host_mode'] = host
        assert_parity(tools, spec, tmp_path, f'poll-{shape}-{host}')


def test_idle_site(tools, tmp_path):
    spec = {'main': image_spec(idle_program())}
    for mode, host in (('fast', 'precise'), ('fast', 'fast'), ('trace', 'fast')):
        spec['mode'], spec['host_mode'] = mode, host
        assert_parity(tools, spec, tmp_path, f'idle-{mode}-{host}')


def test_integer_edges(tools, tmp_path):
    top = 0xFFFFFF00
    # A function at the very top of the address space, ending on a branch with no delay slot.
    top_words = [imm(15, 0, 8, 0x8000), imm(9, 8, 8, -1), imm(4, 0, 0, 0x7FFF), imm(9, 0, 9, 1)] + [0] * 59 + [imm(5, 8, 9, -2)]
    zero_base = [imm(4, 0, 0, 1), 0, imm(5, 8, 0, 0x7FFF), j(3, 0), 0, *RET]
    negative = [imm(5, 8, 0, -0x8000), imm(9, 8, 8, -0x8000), imm(13, 8, 8, 0xFFFF), *RET]
    spec = {'main': image_spec(struct.pack(f'<{len(top_words)}I', *top_words), top),
            'overlays': [image_spec(struct.pack(f'<{len(zero_base)}I', *zero_base), 0, index=7, seeds=[0], scan_calls=False),
                         image_spec(struct.pack(f'<{len(negative)}I', *negative), 0x7FFF0000, index=99, seeds=[0x7FFF0000], scan_calls=False)]}
    for mode, host in (('fast', 'precise'), ('fast', 'fast'), ('trace', 'fast')):
        spec['mode'], spec['host_mode'] = mode, host
        assert_parity(tools, spec, tmp_path, f'edges-{mode}-{host}')


def test_unaligned_image_base(tools, tmp_path):
    words = [imm(9, 0, 2, 7), *RET, 0, 0, *RET]
    blob = bytes(2) + struct.pack(f'<{len(words)}I', *words)
    spec = {'main': image_spec(blob, BASE + 2, BASE + 4, scan_calls=False)}
    assert_parity(tools, spec, tmp_path, 'unaligned')


def test_empty_functions_and_code_ranges(tools, tmp_path):
    assert_parity(tools, {'main': image_spec(bytes(128))}, tmp_path, 'empty')
    data = bytearray(512)
    struct.pack_into('<4I', data, 0, j(3, BASE + 128), 0, *RET)
    struct.pack_into('<I', data, 256, j(3, BASE + 384))
    spec = {'main': image_spec(data, code_ranges=[[BASE, BASE + 128]])}
    assert_parity(tools, spec, tmp_path, 'ranges')
    words = [imm(9, 0, 2, 77), *RET, 0, 0, *RET]
    spec = {'main': image_spec(struct.pack(f'<{len(words)}I', *words), entry_points=[BASE + 24], scan_calls=False)}
    assert_parity(tools, spec, tmp_path, 'entry-points')


def test_catalog_metadata_and_poll_policy(tools, tmp_path):
    data, seeds = split_program(5)
    metadata = {'name': 'movie', 'names': ['movie', 'caf\u00e9 \U0001F3AC'], 'discs': [1, 3],
                'paths': ['data/\u4e16\u754c/a.bin', 'data/"quoted"\\path\t.bin', 'data/\x7fdel'],
                'catalog_sha1': '0' * 40, 'catalog_size': 1234, 'disc': None, 'poll_forward': False}
    overlays = [image_spec(data, BASE, index=index, seeds=seeds, scan_calls=False,
                           metadata=dict(metadata, poll_forward=index == 129, disc=1 if index == 130 else None,
                                         name='movie' if index == 128 else 'other'))
                for index in (128, 129, 130)]
    overlays += [image_spec(data, BASE, index=index, seeds=seeds, scan_calls=False) for index in (27, 59, 33)]
    main_data, main_seeds = split_program(2)
    spec = {'main': image_spec(main_data, seeds=main_seeds, scan_calls=False), 'overlays': overlays}
    assert_parity(tools, spec, tmp_path, 'metadata')


def run_cli(tools, *arguments, cwd):
    _, cli = tools
    result = subprocess.run([str(cli), *map(str, arguments)], capture_output=True, text=True, cwd=cwd, timeout=120)
    assert result.returncode > 0
    assert 'Sanitizer' not in result.stderr and 'runtime error' not in result.stderr, result.stderr
    assert result.stderr.startswith('lionheart-gen: ') or 'usage' in result.stderr, result.stderr
    return result.stderr


def valid_exe(body=b'\0' * 0x400):
    header = bytearray(0x800)
    header[:8] = b'PS-X EXE'
    struct.pack_into('<I', header, 0x10, BASE)
    struct.pack_into('<I', header, 0x18, BASE)
    struct.pack_into('<I', header, 0x1C, len(body))
    return bytes(header) + body


def write_disc(path, sectors):
    data = bytearray(sectors * 2352)
    # A volume descriptor whose root directory record is empty but well formed.
    descriptor = 16 * 2352 + 24
    data[descriptor] = 1
    data[descriptor + 1:descriptor + 6] = b'CD001'
    data[descriptor + 156] = 34
    struct.pack_into('<I', data, descriptor + 156 + 2, 18)
    struct.pack_into('<I', data, descriptor + 156 + 10, 2048)
    data[descriptor + 156 + 25] = 2
    path.write_bytes(bytes(data))


def test_malformed_inputs_end_with_a_message(tools, tmp_path):
    catalog = ROOT / 'games/ff8-usa'
    out = tmp_path / 'out'
    good_cue = tmp_path / 'good.cue'
    empty_bin = tmp_path / 'empty.bin'
    write_disc(empty_bin, 32)
    good_cue.write_text('FILE "empty.bin" BINARY\n  TRACK 01 MODE2/2352\n')

    # A disc image with no FF8 files in its filesystem.
    assert 'expected exactly one' in run_cli(tools, '--disc', good_cue, '--catalog', catalog, '-o', out, cwd=tmp_path)
    assert 'does not identify' in run_cli(tools, '--disc', good_cue, '--legacy-overlays', '--catalog', catalog, '-o', out, cwd=tmp_path)

    # Truncated inputs: a cue with no FILE line, a missing bin, and an image cut short.
    (tmp_path / 'nofile.cue').write_text('REM nothing here\n')
    assert 'no FILE line' in run_cli(tools, '--disc', tmp_path / 'nofile.cue', '--catalog', catalog, '-o', out, cwd=tmp_path)
    (tmp_path / 'missing.cue').write_text('FILE "gone.bin" BINARY\n')
    run_cli(tools, '--disc', tmp_path / 'missing.cue', '--catalog', catalog, '-o', out, cwd=tmp_path)
    truncated = tmp_path / 'truncated.bin'
    truncated.write_bytes(empty_bin.read_bytes()[:18 * 2352 + 10])
    (tmp_path / 'truncated.cue').write_text('FILE "truncated.bin" BINARY\n')
    assert 'truncated' in run_cli(tools, '--disc', tmp_path / 'truncated.cue', '--catalog', catalog, '-o', out, cwd=tmp_path)
    # A directory that claims to extend past the end of the image.
    huge = bytearray(empty_bin.read_bytes())
    struct.pack_into('<I', huge, 16 * 2352 + 24 + 156 + 10, 0x7FFFFFFF)
    (tmp_path / 'huge.bin').write_bytes(bytes(huge))
    (tmp_path / 'huge.cue').write_text('FILE "huge.bin" BINARY\n')
    assert 'truncated' in run_cli(tools, '--disc', tmp_path / 'huge.cue', '--catalog', catalog, '-o', out, cwd=tmp_path)

    # Wrong executables.
    exe = tmp_path / 'wrong.exe'
    exe.write_bytes(b'this is not a PlayStation executable')
    assert 'not a PS-X EXE' in run_cli(tools, exe, '--catalog', catalog, '-o', out, cwd=tmp_path)
    exe.write_bytes(valid_exe())
    assert 'unsupported EXE' in run_cli(tools, exe, '--catalog', catalog, '-o', out, cwd=tmp_path)
    header = bytearray(valid_exe())
    struct.pack_into('<I', header, 0x1C, 0xFFFFFFFC)
    exe.write_bytes(bytes(header))
    assert 'truncated EXE' in run_cli(tools, exe, '--catalog', catalog, '-o', out, cwd=tmp_path)
    exe.write_bytes(b'')
    run_cli(tools, exe, '--catalog', catalog, '-o', out, cwd=tmp_path)

    # Bad catalogs: missing, unparsable, wrong types.
    exe.write_bytes(valid_exe())
    bad = tmp_path / 'bad'
    bad.mkdir()
    assert 'cannot open' in run_cli(tools, exe, '--catalog', bad, '-o', out, cwd=tmp_path)
    for content in ['{"exe_sha1": ', '[1, 2,, 3]', '{"exe_sha1": 12}', '{"exe_sha1": "x"}{}', '"\\ud800"', '{"a": 1e400000}', '[' * 400]:
        (bad / 'overlays.json').write_text(content)
        run_cli(tools, exe, '--catalog', bad, '-o', out, cwd=tmp_path)
    (bad / 'overlays.json').write_bytes(b'{"exe_sha1": "\xff\xfe"}')
    run_cli(tools, exe, '--catalog', bad, '-o', out, cwd=tmp_path)


def test_rerun_keeps_timestamps_and_removes_stale_units(tools, tmp_path):
    tool, _ = tools
    data, seeds = split_program(30)
    spec_path = tmp_path / 'spec.json'
    spec_path.write_text(json.dumps({'main': image_spec(data, seeds=seeds, scan_calls=False)}))
    output = tmp_path / 'out'
    subprocess.run([str(tool), str(spec_path), str(output)], check=True, capture_output=True)
    (output / 'functions_099.c').write_text('stale')
    (output / 'keepme.c').write_text('not ours')
    before = {p.name: p.stat().st_mtime_ns for p in output.iterdir() if p.name != 'functions_099.c'}
    subprocess.run([str(tool), str(spec_path), str(output)], check=True, capture_output=True)
    assert not (output / 'functions_099.c').exists()
    assert (output / 'keepme.c').exists()
    assert {p.name: p.stat().st_mtime_ns for p in output.iterdir()} == before


@pytest.mark.parametrize('timing', ['block', 'loose'])
@pytest.mark.parametrize('fused', [False, True])
def test_timing_regions_and_owners(tools, tmp_path, timing, fused):
    words = [imm(4, 0, 0, 2), imm(43, 20, 8, 0), imm(9, 0, 9, 99),
             imm(9, 0, 9, 3), imm(35, 20, 8, 0), 0, r(8, rs=31), 0]
    data = struct.pack('<8I', *words) * 50
    seeds = [BASE + n * 32 for n in range(50)]
    selected = [f'f_{seeds[0]:08x}', f'f_{seeds[-1]:08x}', f'ov2_f_{BASE:08x}', 'f_00000000']
    spec = {'timing': timing, 'main': image_spec(data, entry_points=seeds, scan_calls=False),
            'overlays': [image_spec(data, index=2, entry_points=seeds, scan_calls=False)],
            'register_cache': dict(max_registers=8, functions=selected, fuse_forward=selected if fused else [])}
    assert_parity(tools, spec, tmp_path, 'regions')
    manifest = json.loads((tmp_path / 'regions-c/manifest.json').read_text())
    if fused:
        assert manifest['forward_regions'] == selected[:2]
    else:
        assert 'forward_regions' not in manifest


@pytest.mark.parametrize('size', [31, 32, 33, 512, 513])
@pytest.mark.parametrize('timing', ['block', 'loose'])
def test_timing_inline_and_region_bounds(tools, tmp_path, size, timing):
    words = [imm(9, 8, 8, 1)] * (size - 2) + RET
    data = struct.pack(f'<{size}I', *words)
    selected = dict(max_registers=8, functions=[f'f_{BASE:08x}'], fuse_forward=[f'f_{BASE:08x}'])
    spec = {'timing': timing, 'main': image_spec(data), 'register_cache': selected}
    if size == 513:
        assert_spec_error(tools, spec, tmp_path, 'exceeds 512 instructions')
        with pytest.raises(ValueError, match='exceeds 512 instructions'):
            python_emit(spec, tmp_path / 'python')
    else:
        assert_parity(tools, spec, tmp_path, 'bounded')
    spec['register_cache'] = dict(max_registers=8, functions=['f_00000000'])
    assert_parity(tools, spec, tmp_path, 'cold')
    source = (tmp_path / 'cold-c/functions_000.c').read_text()
    assert ('#ifdef FF8_TIMING_INLINE' in source) == (size <= 32)


@pytest.mark.parametrize('timing', ['exact', 'block', 'loose'])
def test_timing_entries_and_public_cache_option(tools, tmp_path, timing):
    words = [*RET, 0, 0, imm(9, 8, 8, 1), *RET]
    data = struct.pack('<7I', *words)
    exe = tmp_path / 'program.exe'
    exe.write_bytes(valid_exe(data))
    catalog = tmp_path / 'catalog'
    catalog.mkdir()
    (catalog / 'overlays.json').write_text(json.dumps(dict(exe_sha1=hashlib.sha1(exe.read_bytes()).hexdigest(),
        main_code_ranges=[[hex(BASE), hex(BASE + len(data))]], timing_entry_points=[hex(BASE + 16)])))
    spec = {'timing': timing, 'main': image_spec(data, code_ranges=[[BASE, BASE + len(data)]],
                                               timing_entry_points=[BASE + 16]),
            'exe_path': str(exe), 'catalog_dir': str(catalog),
            'register_cache': dict(max_registers=1, functions=[f'f_{BASE + 16:08x}'])}
    assert_parity(tools, spec, tmp_path, 'public-api')
    source = (tmp_path / 'public-api-c/functions_000.c').read_text()
    assert ('#ifdef FF8_PLAY_REG_CACHE' in source) == (timing != 'exact')


@pytest.mark.parametrize('timing', ['block', 'loose'])
def test_cache_qualification(tools, tmp_path, timing):
    shapes = [(struct.pack('<3I', imm(9, 8, 8, 1), *RET), True),
              (struct.pack('<2I', r(8, rs=8), 0), False),
              (struct.pack('<2I', r(9, rs=8, rd=31), 0), False),
              (jump_table_program(3), True)]
    for index, (data, expected) in enumerate(shapes):
        image = build_image(image_spec(data))
        assert qualify(discover(image)[BASE]) == expected
        spec = {'timing': timing, 'main': image_spec(data),
                'register_cache': dict(max_registers=8, functions=[f'f_{BASE:08x}'])}
        name = f'qualify{index}'
        assert_parity(tools, spec, tmp_path, name)
        source = (tmp_path / f'{name}-c/functions_000.c').read_text()
        assert ('#ifdef FF8_PLAY_REG_CACHE' in source) == expected


@pytest.mark.parametrize('timing', ['block', 'loose'])
def test_wait_hint_and_memory_variants(tools, tmp_path, timing):
    words = [imm(35, 20, 8, 0), 0, imm(5, 8, 0, -3), 0, *RET]
    for opcode in [32, 33, 34, 35, 36, 37, 38, 40, 41, 42, 43, 46, 50, 58]:
        for reg in [0, 1, 7, 15, 23, 28, 31]:
            words += [imm(opcode, 20, reg, 3), *RET]
    data = struct.pack(f'<{len(words)}I', *words)
    seeds = [BASE] + [BASE + n * 4 for n in range(6, len(words), 3)]
    spec = {'timing': timing, 'main': image_spec(data, seeds=seeds, scan_calls=False),
            'register_cache': dict(max_registers=8, functions=[f'f_{pc:08x}' for pc in seeds])}
    assert_parity(tools, spec, tmp_path, 'memory')
    source = (tmp_path / 'memory-c/functions_000.c').read_text()
    assert ('c_timing_idle(c)' in source) == (timing == 'loose')


@pytest.mark.parametrize('cap', range(1, 9))
def test_lowering_register_limit_and_helper_order(tools, tmp_path, cap):
    source = ('void f_80010000(cpu_t *c) {\n' +
              '\n'.join(f'c->r[{reg}] += c->r[{reg}];' for reg in [9, 8, 7, 6, 5, 4, 3, 2, 1]) +
              '\nc->r[9]++;\n'
              'c_timing_block(c, c_merge_value(c, 8), 0, &code_80010000);\n'
              'if (code_valid(c, &code_80010000)) c_tick(c, 0);\n'
              'gte_write(c, 15, ram_read(c, c->r[8], 4), 0);\n'
              'cpu_stop(c, "quote: \\\" ( )");\nreturn;\n}\n')
    expected, registers = lower(source, cap)
    assert registers == cache_registers(source, cap)
    assert registers == tuple(sorted([8, 9, 1, 2, 3, 4, 5, 6, 7][:cap]))
    spec_path = tmp_path / 'lower.json'
    spec_path.write_text(json.dumps(dict(lower_source=source, max_registers=cap)))
    subprocess.run([str(tools[0]), str(spec_path), str(tmp_path / 'native')], check=True, capture_output=True)
    assert (tmp_path / 'native/lowered.c').read_bytes() == expected.encode()


def assert_spec_error(tools, spec, tmp_path, message):
    path = tmp_path / 'invalid.json'
    path.write_text(json.dumps(spec))
    result = subprocess.run([str(tools[0]), str(path), str(tmp_path / 'invalid-output')], capture_output=True, text=True)
    assert result.returncode > 0 and message in result.stderr, result.stderr
    assert 'Sanitizer' not in result.stderr and 'runtime error' not in result.stderr, result.stderr


@pytest.mark.parametrize('source,message', [
    ('void f(cpu_t *c) { c->r[1]++; unknown(c); }', 'unclassified register-cache helper: unknown'),
    ('void f(cpu_t *c) { c->r[1]++; c_tick(c, 0; }', 'unclosed generated helper call'),
])
def test_lowering_rejects_unclassified_or_unclosed_calls(tools, tmp_path, source, message):
    with pytest.raises(ValueError, match=message):
        lower(source)
    assert_spec_error(tools, dict(lower_source=source, max_registers=8), tmp_path, message)


@pytest.mark.parametrize('source', [
    '',
    'void f(cpu_t *c) { c->r[0]++; unknown(c); return; }',
    'void f(cpu_t *c) { c->r[08]++; c_tick(c, 0); return; }',
    'void f(cpu_t *c) { c->r[1]++; cpu_stop(c, "return;"); return; }',
    'void f(cpu_t *c) { c->r[1]++; xcode_valid(c, 0); }',
])
def test_lowering_textual_matches(tools, tmp_path, source):
    if 'xcode_valid' in source:
        with pytest.raises(ValueError, match='unclassified register-cache helper: xcode_valid'):
            lower(source)
        assert_spec_error(tools, dict(lower_source=source, max_registers=8), tmp_path,
                          'unclassified register-cache helper: xcode_valid')
        return
    expected, _ = lower(source)
    path = tmp_path / 'source.json'
    path.write_text(json.dumps(dict(lower_source=source, max_registers=8)))
    subprocess.run([str(tools[0]), str(path), str(tmp_path / 'native')], check=True, capture_output=True)
    assert (tmp_path / 'native/lowered.c').read_bytes() == expected.encode()


@pytest.mark.parametrize('fused,cache', [(False, False), (False, True), (True, False), (True, True)])
def test_native_forward_region_behavior(tools, tmp_path, monkeypatch, fused, cache):
    import test_timing_regions as regions

    def native_emit(image, functions, output, **options):
        spec = {'main': image_spec(image.data, image.base, image.entry, seeds=list(functions)),
                'timing': options['timing'], 'host_mode': options.get('host_mode', 'fast'),
                'register_cache': options['register_cache']}
        path = tmp_path / 'behavior.json'
        path.write_text(json.dumps(spec))
        subprocess.run([str(tools[0]), str(path), str(output)], check=True, capture_output=True)

    monkeypatch.setattr(regions, 'emit', native_emit)
    forward = regions.forward.__wrapped__(SimpleNamespace(param=(fused, cache)), tmp_path)
    regions.test_mmio_flushes_the_complete_forward_prefix(forward)
    regions.test_region_event_changes_survive_cached_continuation(forward)


@pytest.mark.parametrize('selection,message', [
    ([], 'register selection needs'), ({}, 'register selection needs'),
    (dict(max_registers=8, functions=['f_80010000'], extra=1), 'register selection needs'),
    (dict(max_registers=8, functions=['f_80010000'], **{'fuse_forward\0extra': []}), 'register selection needs'),
    *[(dict(max_registers=cap, functions=['f_80010000']), 'max_registers') for cap in [0, 9, True, 1.5]],
    *[(dict(max_registers=8, functions=names), 'functions must') for names in
      [[], ['f_80010000', 'f_80010000'], ['f_8001000A'], ['f_80010000\0'], ['ov_f_80010000'], [1]]],
    *[(dict(max_registers=8, functions=['f_80010000'], fuse_forward=regions), 'fuse_forward') for regions in
      [None, 'f_80010000', {}, [[]], [{}], ['f_80010000', 'f_80010000'], ['f_80010004']]],
])
def test_invalid_cache_selections(tools, tmp_path, selection, message):
    path = tmp_path / 'selection.json'
    path.write_text(json.dumps(selection))
    with pytest.raises(ValueError, match=message):
        read_selection(path)
    assert_spec_error(tools, dict(main=image_spec(bytes(8)), register_cache=selection), tmp_path, message)


@pytest.mark.parametrize('timing', ['block', 'loose'])
def test_experimental_trace_rejected(tools, tmp_path, timing):
    spec = dict(main=image_spec(bytes(8)), timing=timing, mode='trace')
    with pytest.raises(ValueError, match='experimental timing requires fast emission'):
        python_emit(spec, tmp_path / 'python')
    assert_spec_error(tools, spec, tmp_path, 'experimental timing requires fast emission')
    message = run_cli(tools, '--mode', 'trace', '--timing', timing, 'unused.exe', cwd=tmp_path)
    assert 'experimental timing requires fast emission' in message
