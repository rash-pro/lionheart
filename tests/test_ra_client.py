"""Service-client contracts. Every request is confined to a loopback mock."""
import ctypes as C
import os
from pathlib import Path
import shlex
import stat
import subprocess
import time

import pytest

from ra_mock import badge_png, mock_service
from test_host_player import disc

ROOT = Path(__file__).resolve().parents[1]
OFF, SIGNED_OUT, SIGNING_IN, CONNECTED, OFFLINE, REJECTED, UNAVAILABLE = range(7)


@pytest.fixture(scope='module')
def ra_lib(tmp_path_factory):
    out = tmp_path_factory.mktemp('ra-client') / 'client.so'
    vendor = ROOT / 'third_party/rcheevos'
    sources = sorted((vendor / 'src/runtime').glob('*.c'))
    sources += sorted((vendor / 'src/client').glob('*.c'))
    sources += sorted((vendor / 'src/api').glob('*.c'))
    sources += [vendor / 'src' / p for p in ('util/md5.c', 'util/rc_compat.c', 'util/rc_util.c',
                'util/rc_version.c', 'hash/rc_hash.c', 'hash/rc_hash_disc.c', 'hash/rc_cdreader.c')]
    flags = shlex.split(subprocess.check_output(['pkg-config', '--cflags', '--libs', 'sdl2', 'libcurl', 'zlib'], text=True))
    if os.environ.get('FF8_TEST_SANITIZE'):
        flags += ['-fsanitize=address,undefined', '-fno-omit-frame-pointer']
    command = ['cc', '-shared', '-fPIC', '-std=c99', '-O1', '-Wall', '-Wextra', '-Wpedantic', '-Werror',
               '-D_DEFAULT_SOURCE', '-D_POSIX_C_SOURCE=200809L', '-D_FILE_OFFSET_BITS=64',
               '-DFF8_RCHEEVOS', '-DRC_HASH_NO_ROM', '-DRC_HASH_NO_ZIP', '-DRC_HASH_NO_ENCRYPTED',
               '-Iruntime', '-Ibackends/host', f'-I{vendor}/include', f'-I{vendor}/src/api', f'-I{vendor}/src/client',
               'backends/host/ra.c', 'backends/host/ra_http.c', 'backends/host/ra_store.c',
               'backends/host/ra_badge.c', 'backends/host/player_paths.c', 'backends/host/achievements.c',
               'tests/ra_harness.c', *map(str, sources), '-Wl,--wrap=SDL_GetTicks64,--wrap=SDL_CreateThread', '-lm', '-pthread',
               '-o', str(out), *flags]
    result = subprocess.run(command, cwd=ROOT, text=True, capture_output=True)
    assert result.returncode == 0, result.stderr
    lib = C.CDLL(str(out))
    lib.host_ra_configure.argtypes = [C.c_char_p, C.c_int, C.c_int]
    lib.host_ra_login.argtypes = [C.c_char_p, C.c_char_p]
    lib.host_ra_disc.argtypes = [C.c_char_p]
    lib.ra_test_error.restype = C.c_char_p
    lib.ra_test_hash.restype = C.c_char_p
    lib.ra_test_progress.restype = C.c_float
    lib.ra_test_progress_text.restype = C.c_char_p
    lib.ra_test_toast.argtypes = [C.c_double]
    lib.ra_test_toast.restype = C.c_double
    lib.ra_test_png.argtypes = [C.c_void_p, C.c_size_t]
    return lib


def until(lib, predicate, timeout=5):
    end = time.monotonic() + timeout
    while time.monotonic() < end:
        lib.host_ra_idle()
        if predicate():
            return
        time.sleep(.005)
    raise AssertionError(f'client did not settle: state={lib.ra_test_state()}, error={lib.ra_test_error()}')


@pytest.fixture
def client(ra_lib, tmp_path, monkeypatch):
    with mock_service() as service:
        monkeypatch.setenv('LIONHEART_RA_TEST_HOST', service.url)
        monkeypatch.setenv('LIONHEART_RA_TEST_TIMEOUT_MS', '200')
        data = tmp_path / 'data'
        ra_lib.ra_test_reset()
        ra_lib.host_ra_configure(os.fsencode(data), 0, 1)
        try:
            yield ra_lib, service, data
        finally:
            ra_lib.ra_test_reset()


def login(client, password=b'test-password'):
    lib, service, data = client
    lib.host_ra_enable(1)
    lib.host_ra_login(b'TestUser', password)
    until(lib, lambda: lib.ra_test_state() != SIGNING_IN)


def load(client, tmp_path):
    lib, service, data = client
    login(client)
    assert lib.ra_test_state() == CONNECTED
    lib.ra_test_frame(0)
    path = disc(tmp_path / 'synthetic.bin')
    lib.host_ra_disc(os.fsencode(path))
    until(lib, lambda: lib.ra_test_total() == 3 or lib.ra_test_failed() or lib.ra_test_unknown())
    return path


def test_opt_in_login_token_permissions_and_signout(client):
    lib, service, data = client
    lib.host_ra_idle()
    lib.host_ra_login(b'TestUser', b'test-password')
    assert not service.requests and not data.exists()
    login(client)
    assert lib.ra_test_state() == CONNECTED
    token = data / 'ra/credentials'
    assert token.read_text() == 'ra-user=TestUser\nra-token=test-token\n'
    assert stat.S_IMODE(token.stat().st_mode) == 0o600
    assert all(b'test-password' not in p.read_bytes() for p in data.rglob('*') if p.is_file())
    lib.host_ra_configure(os.fsencode(data), 1, 1)
    until(lib, lambda: lib.ra_test_state() == CONNECTED)
    assert service.requests[-1][1]['t'] == 'test-token'
    lib.host_ra_signout()
    assert lib.ra_test_state() == OFF and not token.exists()
    count = len(service.requests)
    lib.ra_test_advance(120000)
    lib.host_ra_idle()
    assert len(service.requests) == count
    for _, _, agent in service.requests:
        assert agent.startswith('Lionheart/') and '(Linux)' in agent and 'rcheevos/' in agent


def test_wrong_password(client):
    login(client, b'wrong')
    lib, _, data = client
    assert lib.ra_test_state() == SIGNED_OUT and lib.ra_test_error() == b'ra.err.credentials'
    assert not (data / 'ra/credentials').exists()


@pytest.mark.parametrize('mode', ['expired', 'blocked', 'blocked-json'])
def test_rejected_token_clears_credentials_and_stops(client, mode):
    lib, service, data = client
    login(client)
    service.mode = mode
    lib.host_ra_configure(os.fsencode(data), 1, 1)
    until(lib, lambda: lib.ra_test_state() == REJECTED)
    assert not (data / 'ra/credentials').exists()
    count = len(service.requests)
    service.mode = 'ok'
    lib.ra_test_advance(120000)
    for _ in range(10):
        lib.host_ra_idle()
    assert len(service.requests) == count
    login(client)
    assert lib.ra_test_state() == CONNECTED


@pytest.mark.parametrize('timeout', [False, True])
def test_offline_and_timeout_login(client, timeout):
    lib, service, data = client
    service.delay = .4 if timeout else 0
    service.mode = 'ok' if timeout else 'offline'
    login(client)
    assert lib.ra_test_state() == OFFLINE and not (data / 'ra/credentials').exists()
    service.delay = 0
    service.mode = 'ok'
    login(client)
    assert lib.ra_test_state() == CONNECTED


def test_token_offline_retries_without_password(client):
    lib, service, data = client
    login(client)
    service.mode = 'offline'
    lib.host_ra_configure(os.fsencode(data), 1, 1)
    until(lib, lambda: lib.ra_test_state() == OFFLINE)
    assert (data / 'ra/credentials').exists()
    service.mode = 'ok'
    lib.ra_test_advance(60001)
    until(lib, lambda: lib.ra_test_state() == CONNECTED)
    assert service.requests[-1][1]['t'] == 'test-token'


def test_unknown_game_and_set_retry(client, tmp_path):
    lib, service, _ = client
    service.set_failure = True
    load(client, tmp_path)
    assert lib.ra_test_failed()
    service.set_failure = False
    service.unknown = True
    lib.host_ra_retry()
    until(lib, lambda: lib.ra_test_unknown())
    assert not lib.ra_test_total()
    service.unknown = False
    lib.host_ra_retry()
    until(lib, lambda: lib.ra_test_total() == 3)


def test_casual_unlock_pending_retry_badges_progress_toast(client, tmp_path):
    lib, service, data = client
    load(client, tmp_path)
    lib.ra_test_frame(0)
    assert lib.ra_test_progress_text(1) == b'0 / 10'
    lib.ra_test_frame(5)
    assert lib.ra_test_progress(1) == 50
    assert lib.ra_test_progress_text(1) == b'5 / 10'
    until(lib, lambda: lib.ra_test_badge(0))
    assert (data / 'ra/badges/101.png').read_bytes() == service.badge
    service.mode = 'offline'
    lib.ra_test_frame(7)
    until(lib, lambda: lib.ra_test_state() == OFFLINE)
    assert lib.ra_test_unlocked() == 1 and lib.ra_test_pending(0)
    assert lib.ra_test_toast(0) == 0
    assert lib.ra_test_toast(60) == pytest.approx(.5)
    assert lib.ra_test_toast(120) == 1
    assert lib.ra_test_toast(1920) == 1
    assert lib.ra_test_toast(2070) == pytest.approx(.5)
    assert lib.ra_test_toast(2220) == -1
    service.mode = 'ok'
    lib.ra_test_advance(120000)
    until(lib, lambda: bool(service.awards))
    until(lib, lambda: lib.ra_test_state() == CONNECTED)
    assert not lib.ra_test_pending(0)
    lib.host_ra_reset()
    lib.ra_test_frame(0)
    lib.ra_test_frame(7)
    assert len(service.awards) == 1
    for _, params, _ in service.requests:
        assert params.get('h', '0') == '0'
        assert params['r'] != 'submitlbentry'


def test_badge_cache_hit_and_corruption(client, tmp_path):
    lib, service, data = client
    path = load(client, tmp_path)
    until(lib, lambda: lib.ra_test_badge(0))
    count = service.badge_requests
    lib.host_ra_configure(os.fsencode(data), 1, 1)
    lib.host_ra_disc(os.fsencode(path))
    until(lib, lambda: lib.ra_test_total() == 3)
    until(lib, lambda: lib.ra_test_badge(0))
    assert service.badge_requests == count
    (data / 'ra/badges/101.png').write_bytes(b'broken')
    lib.host_ra_configure(os.fsencode(data), 1, 1)
    lib.host_ra_disc(os.fsencode(path))
    until(lib, lambda: lib.ra_test_total() == 3)
    until(lib, lambda: lib.ra_test_badge(0))
    assert service.badge_requests == count + 1


def test_logout_during_login_does_not_restore_token(client):
    lib, service, data = client
    service.delay = .3
    lib.host_ra_enable(1)
    lib.host_ra_login(b'TestUser', b'test-password')
    until(lib, lambda: bool(service.requests))
    lib.host_ra_signout()
    time.sleep(.35)
    lib.host_ra_idle()
    assert lib.ra_test_state() == OFF and not (data / 'ra/credentials').exists()


def test_diagnostic_override_fails_closed(ra_lib, tmp_path, monkeypatch):
    monkeypatch.setenv('LIONHEART_RA_TEST_HOST', 'https://example.invalid')
    ra_lib.host_ra_configure(os.fsencode(tmp_path), 1, 1)
    assert ra_lib.ra_test_state() == UNAVAILABLE
    ra_lib.ra_test_reset()


def test_png_validation(ra_lib):
    data = badge_png()
    assert ra_lib.ra_test_png(data, len(data))
    for broken in (b'', data[:30], data[:-12], data[:40] + b'X' + data[41:]):
        assert not ra_lib.ra_test_png(broken, len(broken))


def test_insecure_or_symlinked_credentials_are_not_used(client, tmp_path):
    lib, service, data = client
    login(client)
    token = data / 'ra/credentials'
    token.chmod(0o644)
    count = len(service.requests)
    lib.host_ra_configure(os.fsencode(data), 1, 1)
    lib.host_ra_idle()
    assert lib.ra_test_state() == SIGNED_OUT and len(service.requests) == count
    external = tmp_path / 'external'
    token.rename(external)
    external.chmod(0o600)
    token.symlink_to(external)
    lib.host_ra_configure(os.fsencode(data), 1, 1)
    assert lib.ra_test_state() == SIGNED_OUT and len(service.requests) == count
    lib.host_ra_signout()
    assert not token.is_symlink() and external.exists()


def test_signout_with_an_award_in_flight_and_notification_off(client, tmp_path):
    lib, service, data = client
    load(client, tmp_path)
    lib.host_ra_notify(0)
    lib.ra_test_frame(0)
    service.delay = .3
    lib.ra_test_frame(7)
    until(lib, lambda: any(p['r'] == 'awardachievement' for _, p, _ in service.requests))
    assert lib.ra_test_toast(120) == -1
    lib.host_ra_signout()
    time.sleep(.35)
    lib.host_ra_idle()
    assert lib.ra_test_state() == OFF and not (data / 'ra/credentials').exists()


@pytest.mark.parametrize('mode', ['blocked', 'blocked-json', 'expired'])
def test_award_rejection_is_terminal(client, tmp_path, mode):
    lib, service, data = client
    load(client, tmp_path)
    lib.ra_test_frame(0)
    service.mode = mode
    lib.ra_test_frame(7)
    until(lib, lambda: lib.ra_test_state() == REJECTED)
    count = len(service.requests)
    service.mode = 'ok'
    lib.ra_test_advance(120000)
    for _ in range(5):
        lib.ra_test_frame(0)
        lib.ra_test_frame(7)
    assert len(service.requests) == count and not (data / 'ra/credentials').exists()


def test_disc_change_hashes_each_loaded_disc(client, tmp_path):
    lib, service, data = client
    load(client, tmp_path)
    hashes = [lib.ra_test_hash()]
    for number in (2, 3, 4):
        path = disc(tmp_path / f'synthetic-{number}.bin', number)
        lib.host_ra_disc(os.fsencode(path))
        until(lib, lambda: lib.ra_test_hash() != hashes[-1])
        hashes.append(lib.ra_test_hash())
        until(lib, lambda: any(p['r'] == 'gameid' and p.get('m') == hashes[-1].decode()
                              for _, p, _ in service.requests))
    assert len(set(hashes)) == 4


def test_idle_does_not_evaluate_changed_memory(client, tmp_path):
    lib, _, _ = client
    load(client, tmp_path)
    lib.ra_test_frame(0)
    lib.ra_test_memory(7)
    for _ in range(30):
        lib.host_ra_idle()
    assert lib.ra_test_unlocked() == 0 and lib.ra_test_progress(1) == 0
    lib.ra_test_frame(7)
    assert lib.ra_test_unlocked() == 1


def test_pending_unlock_survives_disc_change(client, tmp_path):
    lib, service, _ = client
    load(client, tmp_path)
    service.award_mode = 'offline'
    lib.ra_test_frame(0)
    lib.ra_test_frame(7)
    until(lib, lambda: lib.ra_test_state() == OFFLINE)
    assert lib.ra_test_pending(0)
    lib.host_ra_disc(os.fsencode(disc(tmp_path / 'disc-two.bin', 2)))
    until(lib, lambda: not lib.ra_test_loading())
    assert lib.ra_test_unlocked() == 1 and lib.ra_test_pending_id(101)
    service.award_mode = 'ok'
    lib.ra_test_advance(120000)
    until(lib, lambda: bool(service.awards) and not lib.ra_test_pending_id(101))


def test_reset_clears_measured_display_and_toast_handles_clock_restart(client, tmp_path):
    lib, _, _ = client
    load(client, tmp_path)
    lib.ra_test_frame(0)
    lib.ra_test_frame(5)
    assert lib.ra_test_progress(1) == 50
    lib.host_ra_reset()
    assert lib.ra_test_progress_text(1) == b'0 / 10' and lib.ra_test_progress(1) == 0
    lib.ra_test_frame(0)
    lib.ra_test_frame(7)
    assert lib.ra_test_toast(5000) == 0
    assert lib.ra_test_toast(5120) == 1
    assert lib.ra_test_toast(0) == 0
    assert lib.ra_test_toast(120) == 1
    assert lib.ra_test_toast(2220) == -1


def test_worker_start_failure_can_sign_out_and_reconfigure(client):
    lib, service, data = client
    lib.ra_test_fail_worker()
    lib.host_ra_configure(os.fsencode(data), 1, 1)
    assert lib.ra_test_state() == UNAVAILABLE
    lib.host_ra_signout()
    lib.host_ra_shutdown()
    assert not service.requests
    lib.host_ra_configure(os.fsencode(data), 1, 1)
    login(client)
    assert lib.ra_test_state() == CONNECTED
