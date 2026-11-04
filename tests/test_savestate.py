"""Fresh-session, bounded versioned states; synthetic data only."""
import hashlib
import os
from pathlib import Path
import struct
import subprocess

import pytest

ROOT = Path(__file__).resolve().parents[1]


@pytest.fixture(scope="module")
def harness(tmp_path_factory):
    output = tmp_path_factory.mktemp("states") / "harness"
    flags = ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"] if os.environ.get("FF8_TEST_SANITIZE") else []
    subprocess.run(["cc", "-std=c99", "-O1", "-g", "-Wall", "-Wextra", "-Wpedantic", "-Werror",
                    "-DFF8_STATS", "-DFF8_SAVESTATES", *flags, "-Iruntime", "tests/savestate_harness.c",
                    *map(str, sorted((ROOT / "runtime").glob("*.c"))), "-o", str(output)], cwd=ROOT, check=True)
    return output


@pytest.mark.parametrize("branch", ["taken", "untaken"])
def test_pipeline_devices_callback_and_private_cards(harness, tmp_path, branch):
    state, card = tmp_path / "state", tmp_path / "card.mcr"
    subprocess.run([str(harness), branch, str(state), str(card)], check=True, timeout=30)
    assert state.read_bytes().startswith(b"FF8SAVE\0")
    subprocess.run([str(harness), "read", str(state)], check=True, timeout=30)


@pytest.mark.parametrize("kind", ["truncate", "checksum", "build", "version", "section_version", "section_size", "trailing", "duplicate", "oversized"])
def test_rejects_without_replacing_guest(harness, tmp_path, kind):
    path = tmp_path / "state"
    subprocess.run([str(harness), "roundtrip", str(path)], check=True, timeout=30)
    data = bytearray(path.read_bytes())
    if kind == "section_version":
        original = bytes(data)
        for target in range(1, 11):
            data = bytearray(original)
            pos = 148
            while struct.unpack_from("<I", data, pos)[0] != target:
                pos += 32 + struct.unpack_from("<I", data, pos + 8)[0]
            struct.pack_into("<I", data, pos + 4, 99)
            data[16:36] = hashlib.sha1(data[36:]).digest()
            path.write_bytes(data)
            result = subprocess.run([str(harness), "read", str(path)], capture_output=True, text=True, timeout=30)
            assert result.returncode == 2 and f"section {target} version 99" in result.stdout
        return
    section = 36 + 112  # identity: build64 + exe/disc/modes12 + disc SHA20 + receipts16
    if kind == "oversized":
        data = bytearray(16 * 1024 * 1024 + 1)
    elif kind == "truncate":
        data = data[:-11]
    elif kind == "checksum":
        data[-7] ^= 0x80
    elif kind == "version":
        struct.pack_into("<I", data, 8, 99)
    else:
        if kind == "build":
            data[36] ^= 1
        elif kind == "section_version":
            struct.pack_into("<I", data, section + 4, 99)
        elif kind == "section_size":
            struct.pack_into("<I", data, section + 8, 0xffffffff)
        elif kind == "duplicate":
            struct.pack_into("<I", data, section, 2)
        elif kind == "trailing":
            data.append(0)
            struct.pack_into("<I", data, 12, len(data))
        data[16:36] = hashlib.sha1(data[36:]).digest()
    path.write_bytes(data)
    result = subprocess.run([str(harness), "read", str(path)], capture_output=True, text=True, timeout=30)
    assert result.returncode == 2 and "rejected:" in result.stdout
    if kind == "build":
        assert "different build" in result.stdout


@pytest.mark.parametrize("old_version", [1, 2])
def test_old_spu_section_is_rejected_without_replacing_guest(harness, tmp_path, old_version):
    path = tmp_path / "state"
    subprocess.run([str(harness), "roundtrip", str(path)], check=True, timeout=30)
    data = bytearray(path.read_bytes())
    pos = 148
    while struct.unpack_from("<I", data, pos)[0] != 6:
        pos += 32 + struct.unpack_from("<I", data, pos + 8)[0]
    assert struct.unpack_from("<I", data, pos + 4)[0] == 3
    # Keep this build's identity and all payload checksums valid so rejection
    # proves the section version check, independently of the build fingerprint.
    struct.pack_into("<I", data, pos + 4, old_version)
    data[16:36] = hashlib.sha1(data[36:]).digest()
    path.write_bytes(data)
    result = subprocess.run([str(harness), "read", str(path)], capture_output=True,
                            text=True, timeout=30)
    assert result.returncode == 2
    assert f"unsupported save state section 6 version {old_version}" in result.stdout


def test_host_hotkeys_replace_only_validated_sessions(tmp_path):
    import shlex
    (tmp_path / "state_build.h").write_text('#define FF8_STATE_BUILD "' + '0' * 64 + '"\n#define FF8_STATE_PRECISE 1\n')
    output = tmp_path / "host-states"
    sdl = shlex.split(subprocess.check_output(["pkg-config", "--cflags", "--libs", "sdl2"], text=True))
    flags = ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"] if os.environ.get("FF8_TEST_SANITIZE") else []
    subprocess.run(["cc", "-std=c99", "-O1", "-g", "-Wall", "-Wextra", "-Wpedantic", "-Werror",
                    "-DFF8_STATS", "-DFF8_SAVESTATES", *flags, "-Iruntime", "-Ibackends/host", "-I" + str(tmp_path),
                    "tests/host_states_harness.c", "backends/host/states.c", "backends/host/state_disc.c", "backends/host/disc_iso.c", "backends/host/input.c", "backends/host/display.c", "backends/host/screenshot.c", "backends/host/osd.c",
                    "backends/host/viewport.c", "backends/host/audio.c",
                    *map(str, sorted((ROOT / "runtime").glob("*.c"))),
                    "-Wl,--wrap=SDL_ClearQueuedAudio,--wrap=SDL_CreateWindow,--wrap=SDL_UpdateTexture,--wrap=host_osd_event", "-o", str(output), *sdl, "-lz"], cwd=ROOT, check=True)
    subprocess.run([str(output), str(tmp_path / "states"), str(tmp_path / "original.mcr")],
                   check=True, timeout=60, env={**os.environ, "SDL_VIDEODRIVER": "dummy", "SDL_AUDIODRIVER": "dummy"})


def test_disc_identity_and_cache(tmp_path):
    image = bytearray(3000 * 2352)

    def sector(lba, data):
        image[lba * 2352 + 15] = 2
        image[lba * 2352 + 24:lba * 2352 + 24 + len(data)] = data

    def record(lba, size, name):
        out = bytearray(34 + len(name))
        out[0] = len(out)
        struct.pack_into("<I", out, 2, lba)
        struct.pack_into("<I", out, 10, size)
        out[32] = len(name)
        out[33:33 + len(name)] = name
        return out

    pvd = bytearray(2048)
    pvd[1:6] = b"CD001"
    struct.pack_into("<I", pvd, 158, 20)
    struct.pack_into("<I", pvd, 166, 2048)
    sector(16, pvd)
    sector(20, record(21, 40, b"SYSTEM.CNF;1") + record(700, 4096, b"SLUS_008.92;1"))
    sector(21, b"BOOT = cdrom:\\SLUS_008.92;1\r\n")
    exe = bytearray(4096)
    exe[:8] = b"PS-X EXE"
    struct.pack_into("<I", exe, 28, 2048)
    exe[2048:] = bytes(range(256)) * 8
    sector(700, exe[:2048])
    sector(701, exe[2048:])
    path = tmp_path / "disc.bin"
    path.write_bytes(image)
    expected = (b"FF8DISC\x01" + struct.pack("<QI", len(image), 0) +
                hashlib.sha1(image[:1024 * 1024]).digest() + hashlib.sha1(image[-1024 * 1024:]).digest() +
                hashlib.sha1(b"SLUS_008.92" + exe).digest() + bytes(16))
    output = tmp_path / "disc-harness"
    flags = ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"] if os.environ.get("FF8_TEST_SANITIZE") else []
    subprocess.run(["cc", "-std=c99", "-O2", "-Wall", "-Wextra", "-Wpedantic", "-Werror", *flags,
                    "-Iruntime", "-Ibackends/host", "tests/state_disc_harness.c", "backends/host/state_disc.c", "backends/host/disc_iso.c",
                    *map(str, sorted((ROOT / "runtime").glob("*.c"))), "-Wl,--wrap=pread", "-o", str(output)], cwd=ROOT, check=True)
    result = subprocess.run([str(output), str(path)], check=True, capture_output=True, text=True, timeout=30)
    assert result.stdout.splitlines()[:2] == [hashlib.sha1(expected).hexdigest(), hashlib.sha1(image).hexdigest()]
