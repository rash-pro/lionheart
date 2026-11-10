"""Shared opt-in owned media discovery. No private paths or data are required."""
from functools import lru_cache
import os
from pathlib import Path
import shutil
import subprocess
import pytest

ROOT = Path(__file__).resolve().parents[1]
GAME_CASES = {
    'test_boot_integration.py': '*',
    'test_gui_pause_player.py': '*',
    'test_gui_shell.py': ['test_shell_recreates_both_renderers_and_keeps_text_input',
                          'test_rescan_relocates_missing_disc_without_replacing_verified_media'],
    'test_host_player.py': ['test_exe_identity_equals_recompiler', 'test_player_contains_no_guest_text'],
    'test_state_bridge.py': ['test_bridge_bind_failure_does_not_stop_game'],
    'test_exactness.py': '*',
}


def pytest_collection_modifyitems(items):
    for item in items:
        selected = GAME_CASES.get(item.path.name, [])
        if selected == '*' or item.originalname in selected:
            item.add_marker(pytest.mark.game)
            if not os.environ.get('LIONHEART_DISCS'):
                item.add_marker(pytest.mark.skip(reason='set LIONHEART_DISCS to run owned-game tests'))


@lru_cache(maxsize=1)
def owned_discs():
    folder = os.environ.get('LIONHEART_DISCS')
    if not folder:
        pytest.skip('set LIONHEART_DISCS to run owned-game tests')
    from tools.psxdisc import Disc
    import json
    identities = json.loads((ROOT / 'games/ff8-usa/overlays.json').read_text())['discs']
    result = {}
    # Follow symlinked disc directories, while rejecting directory cycles.
    seen = set()
    for directory, folders, names in os.walk(folder, followlinks=True):
        real = Path(directory).resolve()
        if real in seen:
            folders[:] = []
            continue
        seen.add(real)
        for name in sorted(names):
            path = Path(directory) / name
            if path.suffix.lower() != '.cue': continue
            disc = Disc(path)
            try:
                paths = {p.strip('/') for p, _ in disc.walk()}
                matches = [int(n) for n, row in identities.items() if row['exe'] in paths and row['image'] in paths]
                if len(matches) == 1:
                    number = matches[0]
                    if number in result: raise ValueError(f'duplicate owned disc {number}')
                    result[number] = path.resolve()
            finally:
                disc.bin.close()
    if 1 not in result:
        pytest.fail('LIONHEART_DISCS does not contain USA disc 1 (SLUS-00892)')
    return result


def owned_disc(number):
    discs = owned_discs()
    if number not in discs: pytest.skip(f'owned USA disc {number} is not supplied')
    return discs[number]


def owned_path(relative):
    owned_discs()
    if relative.startswith('saves/'):
        path = Path(os.environ.get('LIONHEART_SAVES', str(Path(os.environ['LIONHEART_DISCS']) / 'saves'))) / relative.removeprefix('saves/')
        if not path.is_file(): pytest.skip('optional owned memory card is not supplied (LIONHEART_SAVES)')
    else:
        path = ROOT / 'build/generated/extracted' / relative
        if not path.is_file(): pytest.fail('run cmake -P tools/setup.cmake -- --disc1 DISC before owned-game tests')
    return path


@pytest.fixture(scope='session')
def vulkan_replay(tmp_path_factory):
    if not shutil.which('cmake'):
        pytest.skip('CMake is required for the optional Vulkan gate')
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
    binary = out / 'host/gpu_replay'
    probe = subprocess.run([str(binary), '--self-test', '1'], env=dict(os.environ),
                           text=True, capture_output=True, timeout=120)
    if 'Vulkan warning: using native soft presentation' in probe.stderr:
        if os.environ.get('VK_DRIVER_FILES') or os.environ.get('VK_ICD_FILENAMES'):
            pytest.fail('The explicitly selected Vulkan driver is unavailable:\n' + probe.stderr)
        pytest.skip('No usable Vulkan device: ' + probe.stderr)
    assert probe.returncode == 0, probe.stderr
    return binary
