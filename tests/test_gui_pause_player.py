"""Observe the complete CPU object inside the actual player's modal pause loop."""
import hashlib
import json
import os
from pathlib import Path
import re
import shlex
import shutil
import subprocess

import pytest
from conftest import owned_disc, owned_path

from test_host_player import ROOT, real_player


@pytest.fixture(scope='module')
def pause_player(real_player, tmp_path_factory):
    build = real_player.parent
    link = build / 'CMakeFiles/ff8.dir/link.txt'
    if not link.is_file():
        pytest.skip('the release-object pause probe requires the Makefiles CMake generator')
    out = tmp_path_factory.mktemp('pause-player')
    unit = next(row for row in json.loads((build / 'compile_commands.json').read_text())
                if row['file'].endswith('/backends/host/main.c'))
    command = unit.get('arguments') or shlex.split(unit['command'])
    command = list(command)
    command[command.index('-c') + 1] = str(ROOT / 'tests/gui_pause_probe.c')
    command[command.index('-o') + 1] = str(out / 'probe.o')
    # Keep all real ABI/feature flags, without replacing the main object's deps.
    for flag in ('-MF', '-MT', '-MQ'):
        while flag in command:
            index = command.index(flag)
            del command[index:index + 2]
    command = [arg for arg in command if arg not in ('-MD', '-MMD', '-MP')]
    subprocess.run(command, cwd=unit['directory'], check=True, capture_output=True)
    command = shlex.split(link.read_text())
    command = [arg for arg in command if not arg.startswith('-Wl,--dependency-file=')]
    command[command.index('-o') + 1] = str(out / 'pause-player')
    command.insert(1, str(out / 'probe.o'))
    command += ['-Wl,--wrap=cpu_init,--wrap=host_gui_open,--wrap=host_gui_frame']
    subprocess.run(command, cwd=build, check=True, capture_output=True)
    return out / 'pause-player'


@pytest.mark.parametrize('renderer', ['soft', 'vulkan'])
def test_actual_modal_loop_freezes_every_cpu_byte(pause_player, real_player, tmp_path, renderer):
    discs = [owned_disc(1)]
    icd = Path(os.environ.get('FF8_TEST_VULKAN_ICD', '/usr/share/vulkan/icd.d/lvp_icd.json'))
    source = owned_path("saves/disc1-start-maxed.mcr")
    if not discs or not source.is_file() or (renderer == 'vulkan' and not icd.is_file()):
        pytest.skip('owned Disc 1/card and the selected renderer are required')
    original = hashlib.sha256(source.read_bytes()).hexdigest()
    config = tmp_path / 'empty.ini'
    config.write_text('')
    script = tmp_path / 'gui.txt'
    script.write_text(''.join(f'at {frame}\nopen\nwait 4\nclose\n' for frame in (300, 900, 1500)))
    states = []
    for name, binary in [('control', real_player), ('probe', pause_player), ('fault', pause_player)]:
        out = tmp_path / name
        out.mkdir()
        card = out / 'card.mcr'
        shutil.copyfile(source, card)
        command = [str(binary), '--config', str(config), '--headless', '--fast', '--mute',
                   '--stall', '0', '--max-instructions', '0', '--max-vblanks', '2000',
                   '--log', os.devnull, '--disc', str(discs[0]), '--card1', str(card),
                   '--savestate-dir', str(out / 'states'), '--renderer', renderer,
                   '--present-size', '1280x720', '--dump-state', str(out / 'state.bin'),
                   '--input-script', str(ROOT / 'tests/scripts/profile-field.txt')]
        if name != 'control':
            command += ['--gui-test', str(script)]
        env = {**os.environ, 'SDL_VIDEODRIVER': 'dummy', 'SDL_AUDIODRIVER': 'dummy',
               'VK_DRIVER_FILES': str(icd), 'XDG_DATA_HOME': str(out / 'data')}
        if name == 'fault':
            env['FF8_GUI_PAUSE_PROBE_STEP'] = '1'
            env['FF8_GUI_PAUSE_PROBE_FAILURE'] = str(out / 'fault.txt')
        result = subprocess.run(command, cwd=out, text=True, capture_output=True, timeout=180, env=env)
        (out / 'run.log').write_text(result.stdout + result.stderr)
        (out / 'receipt.json').write_text(json.dumps({'command': command, 'exit': result.returncode,
            'binary_sha256': hashlib.sha256(binary.read_bytes()).hexdigest(),
            'injected_retirement': name == 'fault'}, indent=2) + '\n')
        if name == 'fault':
            # An intentional immediate abort cannot drain the player's async
            # stderr tee; the synchronous proof survives without driver cleanup.
            assert result.returncode == 99
            assert (out / 'fault.txt').read_text() == 'guest changed inside modal loop\n'
            continue
        assert result.returncode == 0, result.stdout + result.stderr
        assert 'Stop: VBlank limit reached' in result.stdout
        if name == 'probe':
            proof = re.search(r'GUI pause proof: opens=(\d+) frames=(\d+) frozen=1', result.stderr)
            assert proof and int(proof[1]) == 3 and int(proof[2]) >= 12, result.stderr
            assert 'GUI test complete:' in result.stderr
        states.append((out / 'state.bin').read_bytes())
    assert states[0] == states[1]
    assert hashlib.sha256(source.read_bytes()).hexdigest() == original
