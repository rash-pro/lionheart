"""Host-only telemetry transport and local rcheevos; no disc or RA account needed."""
import ctypes as C
import json
import os
from pathlib import Path
import socket
import signal
import sys
import errno
from contextlib import contextmanager
import subprocess
import time

import pytest
from conftest import owned_disc, owned_path

ROOT = Path(__file__).resolve().parents[1]


@pytest.fixture(scope="module")
def bridge_lib(tmp_path_factory):
    output = tmp_path_factory.mktemp("state-bridge") / "bridge.so"
    vendor = ROOT / "third_party/rcheevos"
    sources = sorted((vendor / "src/runtime").glob("*.c"))
    sources += [vendor / "src" / p for p in (
        "util/md5.c", "util/rc_compat.c", "util/rc_util.c", "api/rc_api_common.c",
        "hash/rc_hash.c", "hash/rc_hash_disc.c", "hash/rc_cdreader.c")]
    sanitize = ["-fsanitize=undefined", "-fno-sanitize-recover=all"] if os.environ.get("FF8_TEST_SANITIZE") else []
    subprocess.run(["cc", "-shared", "-fPIC", "-std=c99", "-O1", "-Wall", "-Wextra", "-Wpedantic", "-Werror",
                    "-D_DEFAULT_SOURCE", "-D_POSIX_C_SOURCE=200809L", "-D_FILE_OFFSET_BITS=64",
                    "-DRC_HASH_NO_ROM", "-DRC_HASH_NO_ZIP", "-DRC_HASH_NO_ENCRYPTED",
                    *sanitize, "-Iruntime", "-Ibackends/host", f"-I{vendor}/include", f"-I{vendor}/src/api",
                    "backends/host/state_bridge.c", "backends/host/ff8_state.c", "backends/host/achievements.c",
                    "tests/state_bridge_harness.c", "tests/state_bridge_syscalls.c", *map(str, sources), "-lm", "-pthread",
                    *["-Wl,--wrap=" + name for name in ("clock_gettime", "socket", "setsockopt", "bind", "listen", "getsockname", "accept", "send", "fsync", "rename")], "-o", str(output)], cwd=ROOT, check=True)
    lib = C.CDLL(str(output))
    lib.test_state_cpu.restype = C.c_void_p
    lib.test_state_free.argtypes = [C.c_void_p]
    lib.test_state_write.argtypes = [C.c_void_p, C.c_uint, C.c_uint, C.c_uint]
    lib.host_state_bridge_open.argtypes = [C.c_char_p]
    lib.host_state_bridge_open.restype = C.c_void_p
    lib.host_state_bridge_frame.argtypes = [C.c_void_p, C.c_void_p, C.c_uint64]
    lib.host_state_bridge_restore.argtypes = [C.c_void_p, C.c_void_p, C.c_uint64]
    lib.host_state_bridge_close.argtypes = [C.c_void_p]
    lib.host_state_bridge_event.argtypes = [C.c_void_p, C.c_char_p]
    lib.test_achievements_open.argtypes = [C.c_char_p, C.c_char_p, C.c_char_p]
    lib.test_achievements_open.restype = C.c_void_p
    lib.host_achievements_frame.argtypes = [C.c_void_p, C.c_void_p]
    lib.host_achievements_close.argtypes = [C.c_void_p]
    lib.host_achievements_read.argtypes = [C.c_uint32, C.c_void_p, C.c_uint32, C.c_void_p]
    lib.test_last_event.restype = C.c_char_p
    lib.test_clock.argtypes = [C.c_double]
    lib.test_network.argtypes = [C.c_int, C.c_int]
    lib.test_client.argtypes = [C.c_int]
    lib.test_send_mode.argtypes = [C.c_int]
    lib.test_sync_fail.argtypes = [C.c_int, C.c_int]
    lib.test_operations.restype = C.c_char_p
    return lib


def patch_file(tmp_path, achievements, **extra):
    path = tmp_path / "patch.json"
    path.write_text(json.dumps({"PatchData": {"Achievements": achievements, **extra}}))
    return path


def test_unlock_and_persistence(bridge_lib, tmp_path):
    lib = bridge_lib
    patch = patch_file(tmp_path, [{"ID": 42, "MemAddr": "0xH000100=7"}])
    progress = tmp_path / "progress.json"
    cpu = lib.test_state_cpu()
    a = lib.test_achievements_open(os.fsencode(patch), os.fsencode(progress), None)
    assert a
    try:
        lib.host_achievements_frame(a, cpu)  # let WAITING become ACTIVE while false
        lib.test_state_write(cpu, 0x100, 7, 1)
        lib.host_achievements_frame(a, cpu)
        assert json.loads(lib.test_last_event()) == {"type": "achievement", "id": 42, "persisted": True}
        assert json.loads(progress.read_text())["unlocked"] == [42]
        lib.host_achievements_frame(a, cpu)
        assert lib.test_events() == 1
    finally:
        assert lib.host_achievements_close(a)
    a = lib.test_achievements_open(os.fsencode(patch), os.fsencode(progress), None)
    assert a
    try:
        lib.test_state_write(cpu, 0x100, 0, 1)
        lib.host_achievements_frame(a, cpu)
        lib.test_state_write(cpu, 0x100, 7, 1)
        lib.host_achievements_frame(a, cpu)
        assert lib.test_events() == 0
    finally:
        lib.host_achievements_close(a)
        lib.test_state_free(cpu)


def test_ps1_linear_memory(bridge_lib):
    lib = bridge_lib
    cpu = lib.test_state_cpu()
    try:
        lib.test_state_write(cpu, 0x1FFFFF, 0xFE, 1)
        lib.test_state_write(cpu, 0x200000, 0x12345678, 4)
        data = (C.c_ubyte * 5)()
        assert lib.host_achievements_read(0x1FFFFF, data, 5, cpu) == 5
        assert bytes(data) == b"\xfe\x78\x56\x34\x12"
        assert lib.host_achievements_read(0x200400, data, 4, cpu) == 0
        assert lib.host_achievements_read(0xFFFFFFFF, data, 4, cpu) == 0
    finally:
        lib.test_state_free(cpu)


@pytest.mark.parametrize("data", ["{", '{"PatchData":{"Achievements":[{"ID":1,"MemAddr":"bad"}]}}',
    '{"PatchData":{"Achievements":[{"ID":1,"MemAddr":"0=1"},{"ID":1,"MemAddr":"0=2"}]}}'])
def test_bad_patch_fails(bridge_lib, tmp_path, data):
    patch = tmp_path / "bad.json"
    patch.write_text(data)
    assert not bridge_lib.test_achievements_open(os.fsencode(patch), os.fsencode(tmp_path / "progress.json"), None)


def test_corrupt_progress_is_preserved(bridge_lib, tmp_path):
    patch = patch_file(tmp_path, [{"ID": 1, "MemAddr": "0xH000100=7"}])
    progress = tmp_path / "progress.json"
    progress.write_text("broken")
    assert not bridge_lib.test_achievements_open(os.fsencode(patch), os.fsencode(progress), None)
    assert progress.read_text() == "broken"


def test_leaderboard_and_rich_presence(bridge_lib, tmp_path):
    lib = bridge_lib
    patch = patch_file(tmp_path, [], Leaderboards=[{"ID": 8,
        "Mem": "STA:0xH000100=1::CAN:0xH000100=2::SUB:0xH000100=3::VAL:0xH000101"}],
        RichPresencePatch="Display:\nLocal \"test\"\n")
    cpu = lib.test_state_cpu()
    a = lib.test_achievements_open(os.fsencode(patch), os.fsencode(tmp_path / "progress.json"), None)
    assert a
    try:
        lib.host_achievements_frame(a, cpu)
        assert json.loads(lib.test_last_event()) == {"type": "rich_presence", "text": 'Local "test"'}
        lib.test_state_write(cpu, 0x100, 1, 1)
        lib.host_achievements_frame(a, cpu)
        assert json.loads(lib.test_last_event())["type"] == "leaderboard"
        lib.test_state_write(cpu, 0x101, 9, 1)
        lib.test_state_write(cpu, 0x100, 3, 1)
        lib.host_achievements_frame(a, cpu)
        assert json.loads(lib.test_last_event())["value"] == 9
    finally:
        lib.host_achievements_close(a)
        lib.test_state_free(cpu)


def connect(port, path):
    s = socket.create_connection(("127.0.0.1", port), timeout=1)
    s.sendall(f"GET {path} HTTP/1.1\r\nHost: localhost\r\n\r\n".encode())
    return s


def receive(s):
    data = bytearray()
    while True:
        try:
            part = os.read(s.fileno(), 65536)
        except (socket.timeout, BlockingIOError):
            break
        if not part:
            break
        data += part
        if b"\n\n" in data:
            break
    return bytes(data)


def test_http_sse_and_changed_snapshots(bridge_lib, capfd):
    lib = bridge_lib
    cpu = lib.test_state_cpu()
    b = lib.host_state_bridge_open(b"0")
    if not b and "Operation not permitted" in capfd.readouterr().err:
        lib.test_state_free(cpu)
        pytest.skip("sandbox forbids bind; inherited-socket transport is tested separately")
    assert b
    import re
    port = int(re.search(r"127.0.0.1:(\d+)", capfd.readouterr().err)[1])
    try:
        with connect(port, "/events") as events:
            lib.host_state_bridge_frame(b, cpu, 1)
            initial = receive(events)
            assert b"text/event-stream" in initial and b"event: state" in initial
            with connect(port, "/state") as s:
                lib.host_state_bridge_frame(b, cpu, 2)
                response = receive(s)
                snapshot = json.loads(response.split(b"\r\n\r\n")[1])
                assert snapshot["vblank"] == 2 and snapshot["schema"] == 1
            lib.host_state_bridge_event(b, b'{"type":"achievement","id":9}')
            assert b'"id":9' in receive(events)
            events.settimeout(.05)
            time.sleep(.26)
            lib.host_state_bridge_frame(b, cpu, 3)
            assert receive(events) == b""  # VBlank metadata alone is not a field change
            with connect(port, "/wrong") as s:
                lib.host_state_bridge_frame(b, cpu, 4)
                assert b"404 Not Found" in receive(s)
    finally:
        lib.host_state_bridge_close(b)
        lib.test_state_free(cpu)


def test_http_and_sse_inherited_socket(bridge_lib):
    lib = bridge_lib
    cpu = lib.test_state_cpu()
    local, remote = socket.socketpair()
    b = lib.host_state_bridge_open(f"fd:{remote.detach()}".encode())
    assert b
    local.setblocking(False)
    try:
        for vblank in (1, 2):
            os.write(local.fileno(), b"GET /state HTTP/1.1\r\nHost: localhost\r\n\r\n")
            lib.host_state_bridge_frame(b, cpu, vblank)
            result = receive(local)
            assert json.loads(result.split(b"\r\n\r\n")[1])["vblank"] == vblank
        os.write(local.fileno(), b"GET /events HTTP/1.1\r\nHost: localhost\r\n\r\n")
        lib.host_state_bridge_frame(b, cpu, 3)
        assert b"event: state" in receive(local)
        lib.host_state_bridge_event(b, b'{"type":"achievement","id":7}')
        assert b'"id":7' in receive(local)
        time.sleep(.26)
        lib.host_state_bridge_frame(b, cpu, 4)
        assert receive(local) == b""
    finally:
        lib.host_state_bridge_close(b)
        lib.test_state_free(cpu)
        local.close()
        remote.close()


def test_snapshot_contract_and_cumulative_kills(bridge_lib):
    lib = bridge_lib
    lib.host_ff8_state_json.argtypes = [C.c_void_p, C.c_void_p, C.c_size_t]
    cpu = lib.test_state_cpu()
    buffer = C.create_string_buffer(16384)
    try:
        lib.host_ff8_state_json(cpu, buffer, len(buffer))
        assert json.loads(buffer.value)["available"] is False
        def put(address, value, size=4):
            lib.test_state_write(cpu, address, value, size)
        put(0x7737A, 0x08FF, 2)
        put(0x781A4, 1, 1)
        put(0x5F158, 1, 2)
        put(0x5F14E, 180, 2)
        put(0xC2568 + 180 * 2, 73, 2)
        put(0xC0908 + 73 * 24, 45474)
        put(0x77E84, 123456)
        put(0x78048, 3481)
        put(0x78054, 49)
        put(0x7805A, 4, 2)
        put(0x77808, 1286, 2)
        put(0x7780A, 9999, 2)
        put(0x7780C, 99000)
        put(0x7789C, 15, 1)
        for i in range(8):
            put(0x77898 + 0x98 * i, i + 1, 2)
        lib.host_ff8_state_json(cpu, buffer, len(buffer))
        state = json.loads(buffer.value)
        assert state["field"] == {"id": 180, "verified": True, "name": "bghoke_2", "name_verified": True}
        assert state["total_kills"] == {"value": 36, "verified": True}
        assert state["battles_won"]["value"] == 49
        assert state["battles_escaped"]["value"] == 4
        assert state["gil"]["value"] == 123456
        assert state["play_time_seconds"]["value"] == 3481
        squall = state["characters"][0]
        assert squall["level"] == {"value": 100, "verified": True}
        assert squall["hp"] == {"value": 1286, "verified": True}
        assert squall["max_hp"]["verified"] is False
        assert state["draw_count"] == {"value": None, "verified": False}
    finally:
        lib.test_state_free(cpu)


def test_sse_coalesces_changed_fields_at_four_hz(bridge_lib):
    lib = bridge_lib
    cpu = lib.test_state_cpu()
    local, remote = socket.socketpair()
    local.setblocking(False)
    b = lib.host_state_bridge_open(f"fd:{remote.detach()}".encode())
    assert b
    try:
        os.write(local.fileno(), b"GET /events HTTP/1.1\r\nHost: localhost\r\n\r\n")
        lib.host_state_bridge_frame(b, cpu, 1)
        assert b"event: state" in receive(local)
        for frame, mode in [(2, 1), (3, 2), (4, 3)]:
            lib.test_state_write(cpu, 0x5F158, mode, 2)
            lib.host_state_bridge_frame(b, cpu, frame)
            assert receive(local) == b""
        time.sleep(.26)
        lib.host_state_bridge_frame(b, cpu, 5)
        message = receive(local)
        state = json.loads(message.split(b"data: ")[1].split(b"\n\n")[0])
        assert state["vblank"] == 5 and state["state"]["mode"]["value"] == "battle"
        lib.host_state_bridge_frame(b, cpu, 6)
        assert receive(local) == b""
    finally:
        lib.host_state_bridge_close(b)
        lib.test_state_free(cpu)
        local.close()
        remote.close()


@pytest.fixture(autouse=True)
def reset_bridge_syscalls(bridge_lib):
    yield
    bridge_lib.test_clock(-1)
    bridge_lib.test_network(0, 0)
    bridge_lib.test_send_mode(0)
    bridge_lib.test_sync_fail(0, 0)

@contextmanager
def inherited(lib):
    cpu = lib.test_state_cpu()
    local, remote = socket.socketpair()
    local.setblocking(False)
    original = remote.detach()
    bridge = lib.host_state_bridge_open(f"fd:{original}".encode())
    assert bridge
    try:
        yield cpu, bridge, local, original
    finally:
        lib.host_state_bridge_close(bridge)
        lib.test_state_free(cpu)
        local.close()


def test_listener_reuses_port_before_bind(bridge_lib):
    lib = bridge_lib
    lib.test_network(1, 0)
    for _ in range(2):
        b = lib.host_state_bridge_open(b"12345")
        assert b and lib.test_reuse_before_bind()
        lib.host_state_bridge_close(b)


def test_inherited_client_survives_idle_and_owns_fd(bridge_lib):
    lib = bridge_lib
    lib.test_clock(100)
    cpu = lib.test_state_cpu()
    local, remote = socket.socketpair()
    original = remote.detach()
    b = lib.host_state_bridge_open(f"fd:{original}".encode())
    assert b
    local.setblocking(False)
    try:
        with pytest.raises(OSError) as error:
            os.fstat(original)
        assert error.value.errno == errno.EBADF
        lib.test_clock(120)
        lib.host_state_bridge_frame(b, cpu, 1)  # idle pump must not expire the client
        with pytest.raises(BlockingIOError):
            os.read(local.fileno(), 1)
        os.write(local.fileno(), b"GET /state HTTP/1.1\r\nHost: localhost\r\n\r\n")
        lib.host_state_bridge_frame(b, cpu, 2)
        assert b"200 OK" in receive(local)
    finally:
        lib.host_state_bridge_close(b)
        lib.test_state_free(cpu)
    assert os.read(local.fileno(), 1) == b""
    local.close()


def test_invalid_addresses_warn_once_after_activation(bridge_lib, tmp_path, capfd):
    lib = bridge_lib
    patch = patch_file(tmp_path, [{"ID": 44, "MemAddr": "0xX300000=1"}])
    cpu = lib.test_state_cpu()
    a = lib.test_achievements_open(os.fsencode(patch), os.fsencode(tmp_path / "progress.json"), None)
    assert a
    try:
        for _ in range(600):
            lib.host_achievements_frame(a, cpu)
        assert capfd.readouterr().err.count("invalid address: id=44") == 1
        assert lib.test_events() == 0
    finally:
        lib.host_achievements_close(a)
        lib.test_state_free(cpu)


def test_stalled_partial_writes_release_all_listener_slots(bridge_lib):
    lib = bridge_lib
    lib.test_network(1, 0)
    lib.test_clock(100)
    lib.test_send_mode(2)  # one partial write followed by EAGAIN
    cpu = lib.test_state_cpu()
    b = lib.host_state_bridge_open(b"12345")
    assert b
    peers = []
    try:
        for _ in range(16):
            local, remote = socket.socketpair()
            local.setblocking(False)
            lib.test_client(remote.detach())
            os.write(local.fileno(), b"GET /state HTTP/1.1\r\nHost: localhost\r\n\r\n")
            peers.append(local)
        lib.host_state_bridge_frame(b, cpu, 1)
        partial = [receive(peer) for peer in peers]
        assert len(partial[0]) == 7 and all(not chunk for chunk in partial[1:])
        for peer in peers:
            with pytest.raises(BlockingIOError):
                os.read(peer.fileno(), 1)
        lib.test_clock(106)
        lib.host_state_bridge_frame(b, cpu, 2)
        for peer in peers:
            receive(peer)
            assert os.read(peer.fileno(), 1) == b""
        lib.test_send_mode(0)
        local, remote = socket.socketpair()
        peers.append(local)
        local.setblocking(False)
        lib.test_client(remote.detach())
        os.write(local.fileno(), b"GET /state HTTP/1.1\r\nHost: 127.0.0.1:12345\r\n\r\n")
        lib.host_state_bridge_frame(b, cpu, 3)
        assert b"200 OK" in receive(local)
    finally:
        for peer in peers:
            peer.close()
        lib.host_state_bridge_close(b)
        lib.test_state_free(cpu)


def test_pipelining_closes_with_an_explicit_error(bridge_lib):
    lib = bridge_lib
    with inherited(lib) as (cpu, b, local, _):
        message = b"GET /state HTTP/1.1\r\nHost: localhost\r\n\r\n"
        os.write(local.fileno(), message + message)
        lib.host_state_bridge_frame(b, cpu, 1)
        response = receive(local)
        assert b"400 Bad Request" in response and b"Connection: close" in response
        assert os.read(local.fileno(), 1) == b""


@pytest.mark.parametrize("host", [b"evil.example", b"localhost.evil.example", b"127.0.0.1:65536",
    b"localhost@evil.example", b"localhost:bad", b"localhost\r\nHost: localhost"])
def test_host_header_rejects_rebinding_and_ambiguity(bridge_lib, host):
    lib = bridge_lib
    with inherited(lib) as (cpu, b, local, _):
        os.write(local.fileno(), b"GET /state HTTP/1.1\r\nHost: " + host + b"\r\n\r\n")
        lib.host_state_bridge_frame(b, cpu, 1)
        assert b"403 Forbidden" in receive(local)
        assert os.read(local.fileno(), 1) == b""


@pytest.mark.parametrize("host", [b"localhost", b"LOCALHOST:8765", b"127.0.0.1:12345"])
def test_local_host_headers_are_accepted(bridge_lib, host):
    lib = bridge_lib
    with inherited(lib) as (cpu, b, local, _):
        os.write(local.fileno(), b"GET /state HTTP/1.1\r\nHost: " + host + b"\r\n\r\n")
        lib.host_state_bridge_frame(b, cpu, 1)
        assert b"200 OK" in receive(local)


def test_socket_disconnect_preserves_sigpipe_disposition(bridge_lib):
    script = r'''
import ctypes as C, os, signal, socket, sys
lib=C.CDLL(sys.argv[1])
lib.test_state_cpu.restype=C.c_void_p
lib.host_state_bridge_open.argtypes=[C.c_char_p];lib.host_state_bridge_open.restype=C.c_void_p
lib.host_state_bridge_frame.argtypes=[C.c_void_p,C.c_void_p,C.c_uint64]
lib.host_state_bridge_close.argtypes=[C.c_void_p];lib.test_state_free.argtypes=[C.c_void_p]
signal.signal(signal.SIGPIPE,signal.SIG_DFL)
a,z=socket.socketpair();fd=z.detach();cpu=lib.test_state_cpu()
b=lib.host_state_bridge_open(f"fd:{fd}".encode());assert b
assert lib.test_sigpipe_is_default()
os.write(a.fileno(),b"GET /state HTTP/1.1\r\nHost: localhost\r\n\r\n");a.close()
lib.host_state_bridge_frame(b,cpu,1)
assert lib.test_sigpipe_is_default()
lib.host_state_bridge_close(b);lib.test_state_free(cpu)
'''
    subprocess.run([sys.executable, "-c", script, bridge_lib._name], check=True)


def test_progress_directory_sync_mode_cleanup_and_recovery(bridge_lib, tmp_path):
    lib = bridge_lib
    patch = patch_file(tmp_path, [{"ID": 42, "MemAddr": "0xH000100=7"}, {"ID": 43, "MemAddr": "0xH000101=7"}])
    progress = tmp_path / "progress.json"
    stale = tmp_path / "progress.json.tmp.ABCDEF"
    dead = tmp_path / "progress.json.tmp.2147483647.ZYXWVU"
    fresh = tmp_path / "progress.json.tmp.GHIJKL"
    unrelated = tmp_path / "other.json.tmp.ABCDEF"
    active = tmp_path / f"progress.json.tmp.{os.getpid()}.MNOPQR"
    link = tmp_path / "progress.json.tmp.STUVWX"
    for p in [stale, dead, fresh, unrelated, active]:
        p.write_text("temporary")
    link.symlink_to(unrelated)
    for p in [stale, unrelated, active]:
        os.utime(p, (time.time() - 172800, time.time() - 172800))
    cpu = lib.test_state_cpu()
    a = None
    try:
        lib.test_operations_clear()
        a = lib.test_achievements_open(os.fsencode(patch), os.fsencode(progress), None)
        assert a and lib.test_operations() == b"FRD"
        assert not stale.exists() and not dead.exists() and fresh.exists() and unrelated.exists() and active.exists() and link.is_symlink()
        lib.host_achievements_close(a)
        a = None
        progress.chmod(0o640)
        a = lib.test_achievements_open(os.fsencode(patch), os.fsencode(progress), None)
        assert a and progress.stat().st_mode & 0o777 == 0o640
        lib.host_achievements_frame(a, cpu)
        lib.test_sync_fail(2, 1)  # file is replaced, directory durability fails
        lib.test_state_write(cpu, 0x100, 7, 1)
        lib.host_achievements_frame(a, cpu)
        assert json.loads(lib.test_last_event())["persisted"] is False
        lib.test_state_write(cpu, 0x101, 7, 1)
        lib.host_achievements_frame(a, cpu)
        assert json.loads(lib.test_last_event())["persisted"] is True
        assert json.loads(progress.read_text())["unlocked"] == [42, 43]
        assert progress.stat().st_mode & 0o777 == 0o640
        assert lib.host_achievements_close(a)
        a = None
    finally:
        if a:
            lib.host_achievements_close(a)
        lib.test_state_free(cpu)


def test_progress_file_sync_failure_keeps_previous_file(bridge_lib, tmp_path):
    lib = bridge_lib
    patch = patch_file(tmp_path, [{"ID": 42, "MemAddr": "0xH000100=7"}])
    progress = tmp_path / "progress.json"
    cpu = lib.test_state_cpu()
    a = lib.test_achievements_open(os.fsencode(patch), os.fsencode(progress), None)
    assert a
    previous = progress.read_bytes()
    try:
        lib.host_achievements_frame(a, cpu)
        lib.test_sync_fail(1, 1)
        lib.test_state_write(cpu, 0x100, 7, 1)
        lib.host_achievements_frame(a, cpu)
        assert progress.read_bytes() == previous
        assert not list(tmp_path.glob("progress.json.tmp.*"))
        assert not lib.host_achievements_close(a)
        a = None
    finally:
        if a:
            lib.host_achievements_close(a)
        lib.test_state_free(cpu)


def test_rich_presence_sse_coalesces_at_four_hz(bridge_lib):
    lib = bridge_lib
    lib.test_clock(100)
    with inherited(lib) as (cpu, b, local, _):
        os.write(local.fileno(), b"GET /events HTTP/1.1\r\nHost: localhost\r\n\r\n")
        lib.host_state_bridge_frame(b, cpu, 1)
        receive(local)
        lib.host_state_bridge_event(b, b'{"type":"rich_presence","text":"first"}')
        assert b'"first"' in receive(local)
        for i in range(1, 5):
            lib.test_clock(100 + i * .04)
            lib.host_state_bridge_event(b, json.dumps({"type": "rich_presence", "text": str(i)}, separators=(",", ":")).encode())
            assert receive(local) == b""
        lib.test_clock(100.26)
        lib.host_state_bridge_frame(b, cpu, 2)
        result = receive(local)
        assert b'"text":"4"' in result and b'"text":"1"' not in result


@pytest.mark.parametrize("text", ["🐱" * 400, "x" + "€" * 400, "a" + "é" * 600])
def test_rich_presence_truncation_keeps_valid_utf8(bridge_lib, tmp_path, text):
    lib = bridge_lib
    patch = patch_file(tmp_path, [], RichPresencePatch="Display:\n" + text + "\n")
    a = lib.test_achievements_open(os.fsencode(patch), os.fsencode(tmp_path / "progress.json"), None)
    cpu = lib.test_state_cpu()
    assert a
    try:
        lib.host_achievements_frame(a, cpu)
        value = json.loads(lib.test_last_event())["text"]
        assert text.startswith(value) and len(value.encode()) <= 1023 and value
    finally:
        lib.host_achievements_close(a)
        lib.test_state_free(cpu)


def test_bridge_bind_failure_does_not_stop_game(tmp_path):
    binary, exe = ROOT / "build/dev/ff8", owned_path("SLUS_008.92")
    if not binary.exists() or not exe.exists():
        pytest.skip("opt-in real host/executable not built")
    shim = tmp_path / "bind-failure.so"
    subprocess.run(["cc", "-shared", "-fPIC", "-std=c99", "-DFF8_BRIDGE_PRELOAD", "tests/state_bridge_syscalls.c", "-o", str(shim)], cwd=ROOT, check=True)
    result = subprocess.run([str(binary), str(exe), "--config", os.devnull, "--headless", "--mute", "--fast", "--state-bridge", "12345", "--max-vblanks", "2"],
        capture_output=True, text=True, env={**os.environ, "LD_PRELOAD": str(shim), "SDL_VIDEODRIVER": "dummy", "SDL_AUDIODRIVER": "dummy"}, timeout=30)
    assert result.returncode == 0 and "VBlank limit reached" in result.stdout
    assert "Warning: state bridge disabled: Address already in use" in result.stderr


def test_title_hides_stale_savemap_and_levels_ignore_unknown_flags(bridge_lib):
    lib = bridge_lib
    lib.host_ff8_state_json.argtypes = [C.c_void_p, C.c_void_p, C.c_size_t]
    cpu = lib.test_state_cpu()
    buffer = C.create_string_buffer(16384)
    try:
        for address, value, size in [(0x7737A, 0x08ff, 2), (0x781A4, 1, 1), (0x5F158, 1, 2), (0x7780C, 99000, 4)]:
            lib.test_state_write(cpu, address, value, size)
        for flag in [0, 1, 15, 254]:
            lib.test_state_write(cpu, 0x7789C, flag, 1)
            lib.host_ff8_state_json(cpu, buffer, len(buffer))
            state = json.loads(buffer.value)
            assert state["available"]
            assert state["characters"][0]["level"] == {"value": 100, "verified": True}
            assert state["disc"]["verified"] is False
        lib.test_state_write(cpu, 0x5F158, 8, 2)
        lib.host_ff8_state_json(cpu, buffer, len(buffer))
        state = json.loads(buffer.value)
        assert not state["available"] and "gil" not in state and "characters" not in state
    finally:
        lib.test_state_free(cpu)


@pytest.mark.parametrize("field,local,lba,name", [(315,136,55638,"dogate_1"),(733,215,69654,"glgateb1")])
def test_additional_replay_field_ids_and_resource_names(bridge_lib, field, local, lba, name):
    lib = bridge_lib
    cpu = lib.test_state_cpu()
    buffer = C.create_string_buffer(16384)
    lib.host_ff8_state_json.argtypes = [C.c_void_p,C.c_void_p,C.c_size_t]
    try:
        for at,value,size in [(0x7737a,0x08ff,2),(0x781a4,1,1),(0x5f158,1,2),(0x5f14e,field,2),(0xc2568+2*field,local,2),(0xc0908+24*local,lba,4)]:
            lib.test_state_write(cpu,at,value,size)
        lib.host_ff8_state_json(cpu,buffer,len(buffer))
        assert json.loads(buffer.value)["field"] == {"id":field,"verified":True,"name":name,"name_verified":True}
    finally:
        lib.test_state_free(cpu)


def test_tcp_restart_after_server_closed_state_reply(bridge_lib, capfd):
    lib = bridge_lib
    cpu = lib.test_state_cpu()
    b = lib.host_state_bridge_open(b"0")
    if not b and "Operation not permitted" in capfd.readouterr().err:
        lib.test_state_free(cpu)
        pytest.skip("sandbox forbids listening TCP sockets; restart ordering has syscall coverage")
    assert b
    import re
    port = int(re.search(r"127.0.0.1:(\d+)",capfd.readouterr().err)[1])
    try:
        with connect(port,"/state") as peer:
            lib.host_state_bridge_frame(b,cpu,1)
            assert b"200 OK" in receive(peer)
            assert os.read(peer.fileno(),1) == b""
    finally:
        lib.host_state_bridge_close(b)
    b = lib.host_state_bridge_open(str(port).encode())
    assert b
    lib.host_state_bridge_close(b)
    lib.test_state_free(cpu)


def test_restore_publishes_same_fields_at_earlier_vblank(bridge_lib):
    lib = bridge_lib
    cpu = lib.test_state_cpu()
    local, remote = socket.socketpair()
    b = lib.host_state_bridge_open(f"fd:{remote.detach()}".encode())
    local.setblocking(False)
    try:
        os.write(local.fileno(), b"GET /events HTTP/1.1\r\nHost: localhost\r\n\r\n")
        lib.host_state_bridge_frame(b, cpu, 500)
        receive(local)
        lib.host_state_bridge_restore(b, cpu, 100)
        assert b'"vblank":100' in receive(local)
    finally:
        lib.host_state_bridge_close(b)
        lib.test_state_free(cpu)
        local.close()


def test_unlock_survives_replacing_guest_session(bridge_lib, tmp_path):
    lib = bridge_lib
    patch = patch_file(tmp_path, [{"ID": 42, "MemAddr": "0xH000100=7"}])
    progress = tmp_path / "progress.json"
    c = lib.test_state_cpu()
    a = lib.test_achievements_open(os.fsencode(patch), os.fsencode(progress), None)
    try:
        lib.host_achievements_frame(a, c)
        lib.test_state_write(c, 0x100, 7, 1)
        lib.host_achievements_frame(a, c)
        assert lib.test_events() == 1
        lib.test_state_free(c)
        c = lib.test_state_cpu()
        lib.host_achievements_frame(a, c)
        lib.test_state_write(c, 0x100, 7, 1)
        lib.host_achievements_frame(a, c)
        assert lib.test_events() == 1
        assert json.loads(progress.read_text())["unlocked"] == [42]
    finally:
        lib.host_achievements_close(a)
        lib.test_state_free(c)
