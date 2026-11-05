"""Render actual sign-in and achievements interactions against the loopback mock."""
import os
from pathlib import Path
import subprocess

import pytest

from ra_mock import mock_service
from test_host_player import ROOT, disc, real_player


@pytest.fixture(scope='module')
def ra_gui(real_player):
    supplied = os.environ.get('LIONHEART_RA_GUI_HOST')
    if supplied:
        return Path(supplied).resolve()
    subprocess.run(['cmake', '--build', str(real_player.parent), '--target', 'ra_gui_contract', '-j4'],
                   check=True, capture_output=True, timeout=300)
    return real_player.parent / 'ra_gui_contract'


def run_gui(binary, tmp, service, steps, *, name, renderer='soft', size=(1280, 720), token=False, unlock=0):
    out = Path(os.environ.get('LIONHEART_RA_GUI_SHOTS', tmp / 'shots')).resolve()
    out.mkdir(parents=True, exist_ok=True)
    data = tmp / 'data'
    if token:
        credentials = data / 'ra/credentials'
        credentials.parent.mkdir(parents=True, exist_ok=True)
        credentials.write_text('ra-user=TestUser\nra-token=test-token\n')
        credentials.chmod(0o600)
    script = tmp / 'gui.txt'
    script.write_text(steps.replace('{shots}', str(out / name)))
    image = disc(tmp / 'synthetic.bin')
    env = {**os.environ, 'SDL_VIDEODRIVER': 'dummy', 'SDL_AUDIODRIVER': 'dummy',
           'LIONHEART_RA_TEST_HOST': service.url, 'LIONHEART_RA_TEST_TIMEOUT_MS': '500',
           'LIONHEART_RA_GUI_ENABLED': '1' if token else '0'}
    if renderer == 'vulkan':
        icd = Path('/usr/share/vulkan/icd.d/lvp_icd.json')
        if not icd.exists():
            pytest.skip('lavapipe is required for headless Vulkan GUI capture')
        env['VK_DRIVER_FILES'] = str(icd)
    result = subprocess.run([str(binary), '--gui-test', str(script), str(data), str(image), renderer,
                             f'{size[0]}x{size[1]}', str(unlock)], cwd=ROOT, env=env,
                            text=True, capture_output=True, timeout=90)
    (out / f'{name}.log').write_text(result.stdout + result.stderr)
    assert result.returncode == 0, result.stdout + result.stderr
    assert 'GUI test complete:' in result.stderr
    assert 'falling back' not in result.stderr and 'using native soft' not in result.stderr
    return out, data


def click(x, y, scale=1):
    return f'mouse {round(x * scale)} {round(y * scale)}\nmouse-down\nmouse-up\nwait 2\n'


def signin(scale=1, password='test-password'):
    return ('open\nwait 3\nsection prev\nwait 3\n'
            'assert-text ra.off "Off"\ncapture {shots}-off.png\n'
            'select\nwait 2\ncapture {shots}-warning.png\nnav right\nselect\nwait 3\n'
            + click(420, 245, scale) + 'text "TestUser"\n'
            + click(420, 340, scale) + f'text "{password}"\nwait 3\ncapture {{shots}}-form.png\n'
            + click(420, 454, scale) + 'wait 90\n')


@pytest.mark.parametrize('renderer,size', [('soft', (1280, 720)), ('soft', (1920, 1080)), ('vulkan', (1280, 720))])
def test_signin_view_search_filters_unlock_and_signout(ra_gui, tmp_path, renderer, size):
    scale = size[1] / 720
    with mock_service() as service:
        steps = (signin(scale) + 'assert-text ra.signed_in "Signed in as TestUser"\n'
                 'capture {shots}-signed-in.png\nselect\nwait 25\n'
                 'close\nat 10\nopen achievements\nwait 15\n'
                 'assert-text ach.stats "0 / 3 unlocked · 0 / 17 points"\n'
                 'assert-text ach.102.status "5 / 10"\n'
                 'capture {shots}-view.png\nnav right\nwait 2\n'
                 'assert-text ach.filters "Unlocked 0"\nnav left\n'
                 + click(920, 100, scale) + 'text "Measured"\nwait 3\n'
                 'assert-text ach.102.title "Measured signal"\ncapture {shots}-search.png\n'
                 'close\nat 55\ncapture {shots}-toast.png\nopen achievements\nwait 15\n'
                 'assert-text ach.stats "1 / 3 unlocked · 5 / 17 points"\n'
                 'capture {shots}-unlocked.png\nsection prev\nsection prev\nwait 3\n'
                 + click(420, 515, scale) + 'wait 3\nassert-text ra.off "Off"\n'
                 'capture {shots}-signed-out.png\nquit\n')
        out, data = run_gui(ra_gui, tmp_path, service, steps, name=f'{renderer}-{size[1]}',
                            renderer=renderer, size=size, unlock=40)
        assert not (data / 'ra/credentials').exists()
        assert any(params['r'] == 'awardachievement' for _, params, _ in service.requests)
        assert all(params.get('h', '0') == '0' for _, params, _ in service.requests)
        assert list(out.glob(f'{renderer}-{size[1]}-*.png'))


def test_offline_cached_view_and_pending_unlock(ra_gui, tmp_path):
    with mock_service() as service:
        service.award_mode = 'offline'
        steps = ('open achievements\nwait 90\nclose\nat 55\nopen achievements\nwait 20\n'
                 'assert-text ach.stats "1 / 3 unlocked · 5 / 17 points"\n'
                 'assert-text ach.pending "· not sent yet"\ncapture {shots}-view.png\n'
                 'section prev\nsection prev\nwait 3\nassert-text ra.offline "Offline, retrying"\n'
                 'capture {shots}-pane.png\nquit\n')
        run_gui(ra_gui, tmp_path, service, steps, name='offline', token=True, unlock=40)


@pytest.mark.parametrize('mode', ['blocked', 'expired', 'offline'])
def test_stored_token_failure_panes(ra_gui, tmp_path, mode):
    with mock_service() as service:
        service.mode = mode
        state = 'ra.offline "Offline, retrying"' if mode == 'offline' else 'ra.rejected "Signed out by retroachievements.org"'
        run_gui(ra_gui, tmp_path, service,
                f'open\nsection prev\nwait 90\nassert-text {state}\ncapture {{shots}}-pane.png\nquit\n',
                name=mode, token=True)


def test_wrong_password_and_load_failure(ra_gui, tmp_path):
    with mock_service() as service:
        run_gui(ra_gui, tmp_path, service, signin(password='wrong') +
                'assert-text ra.err.credentials "Wrong username or password."\ncapture {shots}-pane.png\nquit\n',
                name='bad-password')
    retry = tmp_path / 'retry'
    retry.mkdir()
    with mock_service() as service:
        service.set_failure = True
        run_gui(ra_gui, retry, service,
                'open achievements\nwait 90\nassert-text ach.load.title "Achievements could not be loaded"\n'
                'capture {shots}-dialog.png\nquit\n', name='load-failure', token=True)
