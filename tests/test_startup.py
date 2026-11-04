"""host review regressions: player-only policy and lossless path punctuation."""
import os
from pathlib import Path
import subprocess

import pytest
from recomp.discover import Image
from recomp.emit import emit
from test_host_player import synthetic_player, disc, fake_exe, play, RUN, ROOT


@pytest.fixture(scope='module')
def synthetic_dev(tmp_path_factory):
    tmp = tmp_path_factory.mktemp('host-dev')
    exe = tmp / 'guest.exe'
    exe.write_bytes(fake_exe())
    generated = tmp / 'generated'
    generated.mkdir()
    emit(Image.read(exe), {}, generated)
    build = tmp / 'host'
    subprocess.run(['cmake', '-S', str(ROOT), '-B', str(build),
                    f'-DGENERATED_DIR={generated}', '-DCMAKE_BUILD_TYPE=Release',
                    '-DFF8_PLAYER=OFF', '-DFF8_HOST_INTERPRETER_ONLY=ON',
                    '-DFF8_RCHEEVOS=OFF', '-DFF8_STATE_BRIDGE=OFF'], check=True, capture_output=True)
    subprocess.run(['cmake', '--build', str(build), '--target', 'ff8', '-j8'],
                   check=True, capture_output=True, timeout=180)
    return build / 'ff8', exe


def test_dev_keeps_unidentified_arbitrary_extension_and_order(synthetic_dev, tmp_path):
    binary, exe = synthetic_dev
    # cdrom accepts raw sectors with arbitrary suffix and no PlayStation identity.
    one = tmp_path / 'first.raw'
    two = tmp_path / 'second.data'
    one.write_bytes(bytes(2352 * 32))
    two.write_bytes(b'invalid second image, never opened at startup')
    for paths in [(one,), (one, two)]:
        args = [str(binary), str(exe), '--config', os.devnull, *RUN]
        for path in paths:
            args += ['--disc', str(path)]
        result = subprocess.run(args, capture_output=True, text=True,
                                env={**os.environ, 'SDL_VIDEODRIVER': 'dummy', 'SDL_AUDIODRIVER': 'dummy'})
        assert result.returncode == 0, result.stderr
        assert 'not a PlayStation' not in result.stderr
    reversed_run = subprocess.run([str(binary), str(exe), '--config', os.devnull, *RUN,
                                   '--disc', str(two), '--disc', str(one)],
                                  capture_output=True, text=True,
                                  env={**os.environ, 'SDL_VIDEODRIVER': 'dummy', 'SDL_AUDIODRIVER': 'dummy'})
    assert reversed_run.returncode == 2 and str(two) in reversed_run.stderr


@pytest.mark.parametrize('extension', ['.bin', '.BIN', '.cue', '.CUE'])
def test_positional_disc(synthetic_player, tmp_path, extension):
    image = disc(tmp_path / 'one.bin')
    if extension.lower() == '.cue':
        image = tmp_path / ('one' + extension)
        image.write_text('FILE "one.bin" BINARY\nTRACK 01 MODE2/2352\nINDEX 01 00:00:00\n')
    elif extension == '.BIN':
        image.rename(tmp_path / 'one.BIN')
        image = tmp_path / 'one.BIN'
    result = play(synthetic_player, tmp_path, image, *RUN)
    assert result.returncode == 0, result.stderr


def test_explicit_paths_without_home(synthetic_player, tmp_path):
    image = disc(tmp_path / 'one.bin')
    result = play(synthetic_player, tmp_path, '--disc', image, '--card1', tmp_path / 'card.mcr',
                  '--savestate-dir', tmp_path / 'states', *RUN, HOME='', XDG_DATA_HOME='', XDG_CONFIG_HOME='')
    assert result.returncode == 0, result.stderr


def test_player_path_punctuation(synthetic_player, tmp_path):
    disc(tmp_path / 'FF8 #1.cue.bin')
    cue = tmp_path / 'FF8 #1.cue'
    cue.write_text('FILE "FF8 #1.cue.bin" BINARY\nTRACK 01 MODE2/2352\nINDEX 01 00:00:00\n')
    config = tmp_path / 'settings.ini'
    config.write_text('  # comment\n disc1=FF8 #1.cue\ncard1=card;one.mcr\nfilter=nearest ; comment\n')
    result = play(synthetic_player, tmp_path, '--config', config, *RUN)
    assert result.returncode == 0, result.stderr
    assert (tmp_path / 'card;one.mcr').exists()


@pytest.mark.parametrize('option,value', [('FF8_VULKAN', 'OFF'), ('FF8_PGXP', 'OFF'), ('FF8_STATS', 'ON')])
def test_player_rejects_contradictory_cache(tmp_path, option, value):
    # Existing caches keep their values when option() defaults change.
    (tmp_path / 'CMakeCache.txt').write_text(f'{option}:BOOL={value}\n')
    result = subprocess.run(['cmake', '-S', str(ROOT), '-B', str(tmp_path), '-DFF8_PLAYER=ON',
                             ], capture_output=True, text=True)
    assert result.returncode != 0 and 'FF8_PLAYER requires' in result.stderr


def test_config_path_limit(synthetic_player, tmp_path):
    result = play(synthetic_player, tmp_path, '--config', 'x' * 4096)
    assert result.returncode == 2 and '4095 bytes' in result.stderr


def test_bom_cue_reaches_player_runtime(synthetic_player, tmp_path):
    disc(tmp_path / 'one.bin')
    cue = tmp_path / 'one.cue'
    content = b'\xef\xbb\xbfFILE "one.bin" BINARY\nTRACK 01 MODE2/2352\nINDEX 01 00:00:00\n'
    cue.write_bytes(content)
    result = play(synthetic_player, tmp_path, '--disc', cue, *RUN)
    assert result.returncode == 0, result.stderr
    assert cue.read_bytes() == content


def test_headless_gui_flag_retains_no_disc_error(synthetic_player, tmp_path):
    result = play(synthetic_player, tmp_path, '--gui', *RUN)
    assert result.returncode == 2 and 'Provide FF8 USA disc 1' in result.stderr


def test_long_cli_disc_path_is_not_limited_by_ini_value_storage(synthetic_player, tmp_path):
    folder = tmp_path
    while len(str(folder / 'one.bin')) < 540:
        folder /= 'long-' + 'x' * 70
    folder.mkdir(parents=True)
    image = disc(folder / 'one.bin')
    result = play(synthetic_player, tmp_path, '--disc', image, *RUN)
    assert result.returncode == 0, result.stderr


def test_long_validated_player_setting_round_trips(synthetic_player, tmp_path):
    image = disc(tmp_path / 'one.bin')
    # A path can be valid as a setting without naming an existing movie folder.
    config = tmp_path / 'long-setting.ini'
    config.write_text('screenshot-dir=' + ('a' * 500) + '\n')
    result = play(synthetic_player, tmp_path, '--config', config, '--disc', image, *RUN)
    assert result.returncode == 0, result.stderr


def test_player_path_markers_are_data_for_every_path_option(synthetic_player, tmp_path):
    image = disc(tmp_path / 'one.bin')
    config = tmp_path / 'markers.ini'
    destination = tmp_path / 'screens #1;two'
    config.write_text(f'screenshot-dir={destination} # comment\n')
    result = play(synthetic_player, tmp_path, '--config', config, '--disc', image,
                  '--screenshot-at', '0', *RUN)
    assert result.returncode == 0, result.stderr
    assert len(list(destination.glob('lionheart-*.png'))) == 1
