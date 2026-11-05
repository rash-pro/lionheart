"""Real player shell recreation and text input; no guest or visible window."""
import os
from pathlib import Path
import subprocess

import pytest
from conftest import owned_disc, owned_path

from test_host_player import ROOT, real_player


def test_shell_recreates_both_renderers_and_keeps_text_input(real_player, tmp_path):
    discs = [owned_disc(1)]
    icd = Path(os.environ.get('FF8_TEST_VULKAN_ICD', '/usr/share/vulkan/icd.d/lvp_icd.json'))
    if not discs or not icd.is_file():
        pytest.skip('owned Disc 1 and the selected Vulkan ICD are required for the player shell gate')
    movies = tmp_path / 'movies'
    movies.mkdir()
    config = tmp_path / 'lionheart.ini'
    config.write_text(f'disc1={discs[0]}\n')
    script = tmp_path / 'gui.txt'
    script.write_text('\n'.join([
        'wait 20', 'section next', 'section next', 'section next', 'wait 3',
        'assert-focus video.renderer', f'drop "{movies}"',
        'nav right', 'wait 15', 'assert-text video.renderer "Vulkan"',
        'nav right', 'wait 15', 'assert-text video.renderer "Soft"',
        'section next', 'section next', 'wait 3', 'select', 'wait 2',
        'nav right', 'select', 'wait 3', 'assert-focus ra.user',
        'select', 'wait 2', 'text "sample-user"', 'wait 2',
        'assert-text ra.user "sample-user"', 'quit',
    ]) + '\n')
    result = subprocess.run([str(real_player), '--config', str(config), '--gui',
                             '--gui-test', str(script), '--present-size', '1280x720'],
                            cwd=tmp_path, text=True, capture_output=True, timeout=60,
                            env={**os.environ, 'SDL_VIDEODRIVER': 'dummy', 'SDL_AUDIODRIVER': 'dummy',
                                 'SDL_RENDER_DRIVER': 'software', 'VK_DRIVER_FILES': str(icd),
                                 'XDG_DATA_HOME': str(tmp_path / 'data'), 'XDG_CONFIG_HOME': str(tmp_path / 'config')})
    assert result.returncode == 0, result.stdout + result.stderr
    assert 'GUI test complete:' in result.stderr and 'Vulkan: ' in result.stderr
    assert 'using native soft' not in result.stderr and 'falling back' not in result.stderr
    assert 'Hardware:' not in result.stdout, 'the shell must quit before guest execution'
    text = config.read_text()
    assert 'renderer=soft' in text and str(movies) in text
    assert 'sample-user' not in text


def test_rescan_relocates_missing_disc_without_replacing_verified_media(real_player, tmp_path):
    images = [owned_disc(1)]
    if not images:
        pytest.skip('owned Disc 1 is required for the rescan gate')
    folder = tmp_path / 'discs'
    folder.mkdir()
    for source in images[0].parent.iterdir():
        if source.is_file():
            (folder / source.name).symlink_to(source.resolve())
    config = tmp_path / 'lionheart.ini'
    config.write_text(f'disc1={folder / "missing.cue"}\n')
    script = tmp_path / 'gui.txt'
    script.write_text('\n'.join([
        'wait 20', 'assert-focus disc.1.locate', 'nav left',
        'assert-focus disc.1.rescan', 'select', 'wait 30',
        'assert-text disc.1.status "SLUS-00892 · verified"', 'quit',
    ]) + '\n')
    result = subprocess.run([str(real_player), '--config', str(config), '--gui-test', str(script)],
                            cwd=tmp_path, text=True, capture_output=True, timeout=60,
                            env={**os.environ, 'SDL_VIDEODRIVER': 'dummy', 'SDL_AUDIODRIVER': 'dummy',
                                 'SDL_RENDER_DRIVER': 'software', 'XDG_DATA_HOME': str(tmp_path / 'data'),
                                 'XDG_CONFIG_HOME': str(tmp_path / 'config')})
    assert result.returncode == 0, result.stdout + result.stderr
    assert 'GUI test complete:' in result.stderr
    # The folder contains both its CUE and raw BIN; either verified image is
    # valid, and the later duplicate must leave the first scan result selected.
    assert f'disc1={folder / images[0].with_suffix(".bin").name}' in config.read_text()
    assert 'Hardware:' not in result.stdout


def test_shell_missing_bin_row_names_the_bin(real_player, tmp_path):
    cue = tmp_path / 'selected.cue'
    cue.write_text('FILE "absent.bin" BINARY\nTRACK 01 MODE2/2352\nINDEX 01 00:00:00\n')
    config = tmp_path / 'lionheart.ini'
    config.write_text(f'disc1={cue}\n')
    script = tmp_path / 'gui.txt'
    script.write_text(f'wait 10\nassert-text disc.1.status "File not found: {tmp_path / "absent.bin"}"\nquit\n')
    result = subprocess.run([str(real_player), '--config', str(config), '--gui-test', str(script)],
                            cwd=tmp_path, text=True, capture_output=True, timeout=60,
                            env={**os.environ, 'SDL_VIDEODRIVER': 'dummy', 'SDL_AUDIODRIVER': 'dummy',
                                 'SDL_RENDER_DRIVER': 'software', 'XDG_DATA_HOME': str(tmp_path / 'data'),
                                 'XDG_CONFIG_HOME': str(tmp_path / 'config')})
    assert result.returncode == 0, result.stdout + result.stderr
    assert 'GUI test complete:' in result.stderr and 'Hardware:' not in result.stdout
