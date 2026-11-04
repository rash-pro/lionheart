"""host host GUI contracts: persistence, media, scaling and real SDL script events."""
import json
import os
from pathlib import Path
import re
import shlex
import stat
import subprocess

import pytest

from test_host_player import disc, metadata

ROOT = Path(__file__).resolve().parents[1]
ENV = {**os.environ, 'SDL_VIDEODRIVER': 'dummy', 'SDL_AUDIODRIVER': 'dummy'}


@pytest.fixture(scope='module')
def gui_helpers(tmp_path_factory):
    out = tmp_path_factory.mktemp('gui-helpers')
    (out / 'disc_identity.h').write_text(metadata(out / 'game'))
    sdl = shlex.split(subprocess.check_output(['pkg-config', '--cflags', '--libs', 'sdl2'], text=True))
    binary = out / 'gui'
    sources = ['tests/gui_harness.c', 'backends/host/gui.c', 'backends/host/gui_ini.c',
               'backends/host/gui_media.c', 'backends/host/gui_browser.c',
               'backends/host/options.c', 'backends/host/player_paths.c',
               'backends/host/disc_id.c', 'backends/host/disc_iso.c',
               'runtime/memcard.c', 'runtime/cpu.c']
    subprocess.run(['cc', '-std=c99', '-O1', '-Wall', '-Wextra', '-Wpedantic', '-Werror',
                    '-DFF8_PLAYER', '-DFF8_GUI', '-ffunction-sections', '-fdata-sections', '-Wl,--gc-sections',
                    '-Iruntime', '-Ibackends/host', f'-I{out}', *sources, '-o', str(binary), *sdl],
                   cwd=ROOT, check=True, capture_output=True)
    return binary


def helper(binary, *args, expected=0):
    result = subprocess.run([str(binary), *map(str, args)], env=ENV, text=True,
                            capture_output=True, timeout=30)
    assert result.returncode == expected, (result.stdout, result.stderr)
    return result


def test_writer_keeps_unknown_comments_order_spacing_and_crlf(gui_helpers, tmp_path):
    ini = tmp_path / 'lionheart.ini'
    original = b'# keep this\r\nunknown = untouched # comment\r\n  fullscreen = 0  ; note\r\nrumble=1\r\n'
    ini.write_bytes(original)
    ini.chmod(0o640)
    helper(gui_helpers, 'write', ini, 'fullscreen', '1')
    assert ini.read_bytes() == original.replace(b'fullscreen = 0', b'fullscreen = 1')
    assert stat.S_IMODE(ini.stat().st_mode) == 0o640
    assert not list(tmp_path.glob('*.tmp-*'))


def test_writer_appends_header_and_missing_newline(gui_helpers, tmp_path):
    ini = tmp_path / 'new' / 'nested' / 'lionheart.ini'
    helper(gui_helpers, 'write', ini, 'rumble', '0')
    assert ini.read_text() == '# Lionheart settings\nrumble=0\n'
    assert stat.S_IMODE(ini.stat().st_mode) == 0o600
    ini.write_text('unknown=untouched')
    helper(gui_helpers, 'write', ini, 'fullscreen', '1')
    assert ini.read_text() == 'unknown=untouched\n# Lionheart settings\nfullscreen=1\n'


@pytest.mark.parametrize('prefix', [b'', b'# user comment\r\nunknown = untouched\r\n'])
def test_writer_uses_one_header_across_saved_settings(gui_helpers, tmp_path, prefix):
    ini = tmp_path / 'lionheart.ini'
    ini.write_bytes(prefix)
    helper(gui_helpers, 'write', ini, 'renderer', 'vulkan')
    helper(gui_helpers, 'write', ini, 'internal-scale', '8')
    helper(gui_helpers, 'write', ini, 'renderer', 'soft')
    helper(gui_helpers, 'write', ini, 'filter', 'linear')
    assert ini.read_bytes() == (prefix + b'# Lionheart settings\nrenderer=soft\n'
                               b'internal-scale=8\nfilter=linear\n')


@pytest.mark.parametrize('key,value', [('fullscreen', '2'), ('internal-scale', '9'),
                                      ('renderer', 'metal'), ('not-a-key', '1'),
                                      ('screenshot-dir', 'bad\nfullscreen=1')])
def test_writer_rejects_invalid_values_without_replacing_file(gui_helpers, tmp_path, key, value):
    ini = tmp_path / 'lionheart.ini'
    original = b'# no mutation\nfullscreen=0\n'
    ini.write_bytes(original)
    helper(gui_helpers, 'write', ini, key, value, expected=2)
    assert ini.read_bytes() == original


def test_writer_updates_duplicates_and_keeps_media_hash_name(gui_helpers, tmp_path):
    ini = tmp_path / 'lionheart.ini'
    ini.write_text('fullscreen=0\n# middle\nfullscreen = 0 ; effective\ndisc1=FF8 #1.cue # note\n')
    helper(gui_helpers, 'write', ini, 'fullscreen', '1')
    assert ini.read_text().startswith('fullscreen=1\n# middle\nfullscreen = 1 ; effective\n')
    helper(gui_helpers, 'write', ini, 'disc1', 'FF8 #2.cue')
    assert ini.read_text().endswith('disc1=FF8 #2.cue # note\n')


def test_write_failure_keeps_valid_session_value(gui_helpers, tmp_path):
    blocker = tmp_path / 'file'
    blocker.write_text('keep')
    result = helper(gui_helpers, 'write', blocker / 'lionheart.ini', 'fullscreen', '1', expected=2)
    assert 'ok=0 fullscreen=1' in result.stdout
    assert blocker.read_text() == 'keep'


@pytest.mark.parametrize('height,expected', [(100, .75), (540, .75), (720, 1),
                                           (1080, 1.5), (1440, 2), (2160, 3), (4000, 4)])
def test_scale_math(gui_helpers, height, expected):
    assert float(helper(gui_helpers, 'scale', height).stdout) == expected


def synthetic_card():
    data = bytearray(131072)
    data[:2] = b'MC'
    for block in range(1, 16):
        data[block * 128] = 0xa0
    for block, flags, name in [(1, 0x51, b'BASLUS-00892FF8'), (2, 0x52, b'BASLUS-00892CHAIN'),
                                (3, 0x53, b''), (4, 0x51, b'BASLUS-00908OTHER'),
                                (5, 0x51, b'BASLUS-0089XBAD'), (6, 0xa1, b'BASLUS-00892DELETED'),
                                (7, 0x51, b'BASLUS-00899SAVE')]:
        at = block * 128
        data[at] = flags
        data[at + 10:at + 10 + len(name)] = name
    return bytes(data)


def test_card_summary_counts_blocks_and_ff8_first_directory_frames(gui_helpers, tmp_path):
    card = tmp_path / 'synthetic.mcr'
    card.write_bytes(synthetic_card())
    assert helper(gui_helpers, 'card-summary', card).stdout == '6 2\n'


@pytest.mark.parametrize('size', [0, 1, 131071, 131073, 262144])
def test_card_import_rejects_every_other_size(gui_helpers, tmp_path, size):
    source = tmp_path / 'source.mcr'
    source.write_bytes(bytes(size))
    helper(gui_helpers, 'card-import', source, tmp_path / 'data', expected=2)
    assert source.read_bytes() == bytes(size)
    assert not list((tmp_path / 'data').rglob('*'))


def test_card_import_exact_size_any_extension_is_copy_and_collision_safe(gui_helpers, tmp_path):
    source = tmp_path / 'source.anything'
    original = bytes(range(256)) * 512
    source.write_bytes(original)
    dest = Path(helper(gui_helpers, 'card-import', source, tmp_path / 'data').stdout.strip())
    assert dest == tmp_path / 'data/cards/source.anything'
    assert dest.read_bytes() == original and source.read_bytes() == original
    dest.write_bytes(bytes(131072))
    second = Path(helper(gui_helpers, 'card-import', source, tmp_path / 'data').stdout.strip())
    assert second != dest and second.read_bytes() == original
    assert dest.read_bytes() == bytes(131072) and source.read_bytes() == original


def test_new_card_is_formatted_and_existing_card_is_preserved(gui_helpers, tmp_path):
    data = tmp_path / 'data'
    card = Path(helper(gui_helpers, 'card-new', data, 0).stdout.strip())
    assert card == data / 'cards/card1.mcr'
    original = card.read_bytes()
    assert len(original) == 131072 and original[:2] == b'MC'
    runtime = tmp_path / 'runtime.mcr'
    helper(gui_helpers, 'runtime-card', runtime)
    assert original == runtime.read_bytes()
    assert helper(gui_helpers, 'card-summary', card).stdout == '0 0\n'
    helper(gui_helpers, 'card-new', data, 0)
    assert card.read_bytes() == original


@pytest.mark.parametrize('kind,expected', [(0, ['a.cue', 'B.BIN']), (1, ['c.mcd', 'd.MCR', 'e.srm']),
                                         (2, []), (3, [])])
def test_browser_folders_first_case_insensitive_filters(gui_helpers, tmp_path, kind, expected):
    (tmp_path / 'z-folder').mkdir()
    (tmp_path / 'A-folder').mkdir()
    for name in ('a.cue', 'B.BIN', 'c.mcd', 'd.MCR', 'e.srm', 'hidden.iso'):
        (tmp_path / name).write_bytes(b'test')
    rows = [line.split('\t') for line in helper(gui_helpers, 'browser', tmp_path, kind, 0).stdout.splitlines()]
    assert [(row[0], row[1]) for row in rows] == [('d', 'A-folder'), ('d', 'z-folder')] + [('f', name) for name in expected]
    all_rows = helper(gui_helpers, 'browser', tmp_path, kind, 1).stdout
    if kind < 2:
        assert 'hidden.iso' in all_rows
    else:
        assert 'hidden.iso' not in all_rows


def test_browser_up_and_state_roundtrip(gui_helpers, tmp_path):
    folder = tmp_path / 'space name' / 'nested'
    folder.mkdir(parents=True)
    assert helper(gui_helpers, 'browser-up', folder).stdout.strip() == str(folder.parent)
    data = tmp_path / 'data'
    assert helper(gui_helpers, 'gui-state', data, folder, 2).stdout == f'2\t{folder}\n'
    assert (data / 'gui-state.ini').read_text() == f'last-folder={folder}\nlast-input=pad\n'


@pytest.mark.parametrize('message,id', [
    ('x: No such file or directory', 'disc.err.missing'),
    ('x: unsupported image', 'disc.err.unsupported'),
    ('x: not a PlayStation disc', 'disc.err.not_psx'),
    ('Another game: SCUS_123.45', 'disc.err.other_game'),
    ('Only the USA release is supported', 'disc.err.region'),
    ('EXE FNV-1a check failed', 'disc.err.hash'),
    ('IMG SHA-1 check failed', 'disc.err.img_hash'),
    ('IMG size check failed', 'disc.err.size'),
    ('out of memory', 'disc.err.memory'),
])
def test_disc_error_copy_mapping(gui_helpers, message, id):
    assert helper(gui_helpers, 'disc-error', message).stdout.strip() == id


@pytest.mark.parametrize('number', [1, 2, 3, 4])
def test_media_worker_delivers_identified_serial_and_generation(gui_helpers, tmp_path, number):
    path = disc(tmp_path / 'image.bin', number)
    result = helper(gui_helpers, 'identify', path, 0).stdout.split('\t')
    assert result[:3] == [str(number), '91', '2']  # GUI_DISC_VERIFIED
    assert result[3] == str(path)


def test_folder_scan_identifies_each_filtered_file(gui_helpers, tmp_path):
    for n in (4, 2, 1):
        disc(tmp_path / f'disc{n}.bin', n)
    (tmp_path / 'ignored.iso').write_bytes(b'not scanned')
    rows = helper(gui_helpers, 'scan', tmp_path, 3).stdout.splitlines()
    assert sorted(int(row.split('\t')[0]) for row in rows) == [1, 2, 4]
    assert all(row.split('\t')[1:3] == ['37', '2'] for row in rows)


def test_string_table_covers_each_static_ui_reference(gui_helpers):
    header = (ROOT / 'backends/host/gui_strings.h').read_text()
    ids = re.findall(r'\{\s*"([a-z][a-z0-9_.-]+)"\s*,', header)
    assert ids and len(ids) == len(set(ids))
    sources = list((ROOT / 'backends/host').glob('gui*.c')) + list((ROOT / 'backends/host').glob('gui*.cpp'))
    used = {id for source in sources for id in re.findall(r'(?:host_gui_string|S)\("([a-z][a-z0-9_.-]+)"\)', source.read_text())}
    assert used <= set(ids), sorted(used - set(ids))
    assert helper(gui_helpers, 'string', 'app.name').stdout == 'Lionheart\n'
    assert helper(gui_helpers, 'string', 'ra.err.unavailable').stdout == 'Not available in this build.\n'


@pytest.fixture(scope='module')
def gui_script(tmp_path_factory):
    binary = tmp_path_factory.mktemp('gui-script') / 'script'
    sdl = shlex.split(subprocess.check_output(['pkg-config', '--cflags', '--libs', 'sdl2'], text=True))
    subprocess.run(['cc', '-std=c99', '-Wall', '-Wextra', '-Wpedantic', '-Werror', '-Ibackends/host',
                    'tests/gui_script_harness.c', 'backends/host/gui_test.c', '-o', str(binary), *sdl],
                   cwd=ROOT, check=True, capture_output=True)
    return binary


def script_run(binary, tmp, script, expected=0):
    path = tmp / 'steps.txt'
    path.write_text(script)
    return helper(binary, path, expected=expected)


def test_gui_test_injects_balanced_real_sdl_events(gui_script, tmp_path):
    result = script_run(gui_script, tmp_path, 'at 3\nopen\nnav down\nnav up\nnav left\nnav right\nselect\n'
                        'section next\nsection prev\ntext "héllo"\ndrop "/tmp/path with spaces.cue"\n'
                        'wait 2\nassert-focus btn.browse\nassert-text app.name "Lionheart"\n'
                        f'capture "{tmp_path}/capture.png"\nclose\n')
    assert 'keys=9 ups=9 text=6 drops=1' in result.stdout
    assert 'finished=1 failed=0 visible=0' in result.stdout


@pytest.mark.parametrize('script', ['not-a-command\n', 'open\nnav diagonal\n', 'wait -1\n',
                                    'open\nassert-focus wrong\n', 'open\nassert-text app.name "Wrong"\n',
                                    'capture /tmp/hidden.png\n', 'open\ntext "unterminated\n',
                                    'open\nmouse nope 1\n', 'open\nmouse 1 2 junk\n', 'select-up\n',
                                    'mouse-up\n', 'open\nmouse-down\n', 'open\nselect-down\n',
                                    'nav down\n', 'section next\n', 'text "hidden"\n',
                                    'drop "/tmp/hidden.cue"\n'])
def test_gui_test_fails_closed_on_invalid_steps_or_observations(gui_script, tmp_path, script):
    result = script_run(gui_script, tmp_path, script, expected=2)
    assert 'failed=1' in result.stdout and 'GUI test' in result.stderr


def test_gui_test_queues_balanced_held_and_mouse_input(gui_script, tmp_path):
    result = script_run(gui_script, tmp_path, 'open\nmouse 400 200\nmouse-down\nwait 2\nmouse-up\n'
                        'select-down\nwait 2\nselect-up\nclose\n')
    assert 'finished=1 failed=0 visible=0' in result.stdout


def test_audio_pause_preserves_device_queue_preroll_and_resumed_playback(tmp_path):
    binary = tmp_path / 'gui-audio'
    sdl = shlex.split(subprocess.check_output(['pkg-config', '--cflags', '--libs', 'sdl2'], text=True))
    subprocess.run(['cc', '-std=c99', '-O1', '-Wall', '-Wextra', '-Werror', '-Iruntime',
                    'tests/gui_audio_harness.c', 'runtime/spu.c', '-o', str(binary), *sdl],
                   cwd=ROOT, check=True, capture_output=True)
    result = subprocess.run([str(binary)], env=ENV, cwd=ROOT, text=True, capture_output=True, timeout=20)
    assert result.returncode == 0, result.stderr
    assert 'resumed playback stayed started' in result.stdout


def test_gui_pause_real_controller_events_and_guest_sio_wire(tmp_path):
    binary = tmp_path / 'gui-input'
    sdl = shlex.split(subprocess.check_output(['pkg-config', '--cflags', '--libs', 'sdl2'], text=True))
    sources = ['tests/gui_input_harness.c', 'backends/host/display.c', 'backends/host/input.c',
               'backends/host/screenshot.c', 'backends/host/osd.c', 'backends/host/viewport.c',
               *map(str, sorted((ROOT / 'runtime').glob('*.c')))]
    flags = ['-fsanitize=address,undefined', '-fno-omit-frame-pointer'] if os.environ.get('FF8_TEST_SANITIZE') else []
    subprocess.run(['cc', '-std=c99', '-O1', '-g', '-Wall', '-Wextra', '-Werror', '-DFF8_GUI',
                    '-Iruntime', '-Ibackends/host', *flags, *sources, '-o', str(binary), *sdl, '-lz'],
                   cwd=ROOT, check=True, capture_output=True)
    result = subprocess.run([str(binary)], env=ENV, cwd=ROOT, text=True, capture_output=True, timeout=30)
    assert result.returncode == 0, result.stdout + result.stderr
    assert 'Guide taps opened; chords never opened; paused SIO replies were released; held input cleared' in result.stdout


@pytest.fixture(scope='module')
def gui_adapter(tmp_path_factory):
    output = tmp_path_factory.mktemp('gui-adapter')
    subprocess.run(['python3', 'tools/embed_gui_assets.py', str(output / 'gui_assets.h')],
                   cwd=ROOT, check=True, capture_output=True)
    sdl = shlex.split(subprocess.check_output(['pkg-config', '--cflags', '--libs', 'sdl2'], text=True))
    flags = ['-std=c++17', '-O0', '-Wall', '-Wextra', '-Werror',
             '-Ibackends/host', '-Ithird_party/imgui', f'-I{output}']
    sources = ['backends/host/gui_imgui.cpp',
               *[str(ROOT / 'third_party/imgui' / source) for source in
                 ('imgui.cpp', 'imgui_draw.cpp', 'imgui_widgets.cpp', 'imgui_tables.cpp',
                  'backends/imgui_impl_sdl2.cpp', 'backends/imgui_impl_sdlrenderer2.cpp')]]
    objects = []
    for index, source in enumerate(sources):
        obj = output / f'adapter-{index}.o'
        subprocess.run(['c++', *flags, '-c', source, '-o', str(obj), *sdl],
                       cwd=ROOT, check=True, capture_output=True, timeout=180)
        objects.append(str(obj))
    binaries = {}
    for name, harness in [('held-entry', 'tests/gui_held_entry_harness.cpp'), ('navigation', 'tests/gui_nav_harness.cpp')]:
        binary = output / name
        subprocess.run(['c++', *flags, harness, *objects, '-o', str(binary), *sdl],
                       cwd=ROOT, check=True, capture_output=True, timeout=90)
        binaries[name] = binary
    return binaries


def test_controller_held_on_entry_cannot_activate_resume(gui_adapter):
    result = subprocess.run([str(gui_adapter['held-entry'])], cwd=ROOT, text=True, capture_output=True, timeout=30,
                            env={**ENV, 'SDL_VIDEODRIVER': 'offscreen', 'SDL_RENDER_DRIVER': 'software'})
    assert result.returncode == 0, result.stdout + result.stderr
    assert 'actions_from_button_held_on_entry=0' in result.stdout
    assert 'actions_after_release_and_fresh_press=1' in result.stdout


def test_keyboard_navigation_stays_in_pane_and_pad_text_switch_opens_keyboard(gui_adapter):
    result = subprocess.run([str(gui_adapter['navigation'])], cwd=ROOT, text=True, capture_output=True, timeout=30,
                            env={**ENV, 'SDL_VIDEODRIVER': 'offscreen', 'SDL_RENDER_DRIVER': 'software'})
    assert result.returncode == 0, result.stdout + result.stderr
    assert 'down_from_renderer=filter' in result.stdout
    assert 'same_field_pad_callbacks=1' in result.stdout


def test_hidden_gui_assertion_fails_during_event_tick(gui_script, tmp_path):
    result = script_run(gui_script, tmp_path, 'assert-focus btn.browse\n', expected=2)
    assert 'interaction or assertion requires a visible GUI' in result.stderr
    assert 'ticks=1 ' in result.stdout
