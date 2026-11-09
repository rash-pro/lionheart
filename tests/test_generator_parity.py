"""Byte parity between the C generator and the Python recompiler, and its error handling."""
import json
import os
from pathlib import Path
import random
import shutil
import struct
import subprocess

import pytest

from recomp.discover import Image, discover
from recomp.emit import emit
from test_recompiler import BASE, CASES, PCS, RET, imm, j, r, table, table_pc

ROOT = Path(__file__).resolve().parents[1]
SANITIZE = bool(os.environ.get('FF8_TEST_SANITIZE'))


@pytest.fixture(scope='session')
def tools():
    build = ROOT / ('build/generator-test-sanitize' if SANITIZE else 'build/generator-test')
    configure = ['cmake', '-S', str(ROOT / 'generator'), '-B', str(build), '-DCMAKE_BUILD_TYPE=Release']
    if SANITIZE:
        configure.append('-DLIONHEART_GENERATOR_SANITIZE=ON')
    subprocess.run(configure, check=True, capture_output=True)
    subprocess.run(['cmake', '--build', str(build), '-j', '4'], check=True, capture_output=True)
    return build / 'lionheart-gen-spec', build / 'lionheart-gen'


def image_spec(data, base=BASE, entry=None, **extra):
    return {'data': bytes(data).hex(), 'base': base, 'entry': base if entry is None else entry, **extra}


def build_image(row):
    ranges = tuple(tuple(pair) for pair in row.get('code_ranges', ()))
    image = Image(bytes.fromhex(row['data']), row['base'], row['entry'],
                  code_ranges=ranges, entry_points=tuple(row.get('entry_points', ())))
    if 'metadata' in row:
        image.metadata = row['metadata']
    return image


def python_emit(spec, output):
    main = spec['main']
    image = build_image(main)
    functions = discover(image, main.get('seeds', ()), main.get('scan_calls', True))
    overlays = []
    for row in spec.get('overlays', ()):
        overlay = build_image(row)
        overlays.append((row['index'], overlay, discover(overlay, row.get('seeds', ()), row.get('scan_calls', True))))
    emit(image, functions, output, mode=spec.get('mode', 'fast'), overlays=overlays,
         host_mode=spec.get('host_mode', 'precise'))


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
            mismatch = next(i for i, (a, b) in enumerate(zip(actual[key], expected[key])) if a != b)
            pytest.fail(f'{name}: {key} differs at byte {mismatch}')


def test_recompiler_cases(tools, tmp_path):
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
        spec = {'mode': mode, 'host_mode': host, 'main': image_spec(data, seeds=seeds), 'overlays': overlays}
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
def test_random_programs(tools, tmp_path, seed):
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
    overlay_data = bytearray(struct.pack(f'<{count // 2}I', *[random_word(rng, 0x80098000, count // 2) for _ in range(count // 2)]))
    spec = {'mode': mode, 'host_mode': host,
            'main': image_spec(data, base, base + rng.randrange(count) * 4, seeds=seeds, scan_calls=rng.random() < 0.5),
            'overlays': [image_spec(overlay_data, 0x80098000, index=rng.choice([0, 1, 27, 33, 70]),
                                    seeds=[0x80098000], scan_calls=False)]}
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
