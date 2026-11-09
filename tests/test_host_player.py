"""Player distribution boundary and synthetic ISO9660 startup contracts."""
import hashlib
import json
import os
from pathlib import Path
import random
import re
import shutil
import struct
import subprocess
import sys

import pytest
from conftest import owned_disc, owned_path

from recomp.discover import Image
from recomp.emit import emit, fnv1a
from tools.check_player_binary import check
from tools.disc_identity_header import header

ROOT = Path(__file__).resolve().parents[1]
SERIALS = ["SLUS_008.92", "SLUS_009.08", "SLUS_009.09", "SLUS_009.10"]


def fake_exe():
    data = bytearray(4096)
    data[:8] = b"PS-X EXE"
    struct.pack_into("<4I", data, 16, 0x80010000, 0, 0x80010000, 2048)
    data[2048:] = random.Random(14).randbytes(2048)
    struct.pack_into("<2I", data, 2048, 0x1000FFFF, 0)  # branch to self, NOP delay
    return bytes(data)


def metadata(game):
    game.mkdir()
    exe = fake_exe()
    (game / "exe-identity.json").write_text(json.dumps({
        "base": 0x80010000, "size": 2048, "fnv1a": f"{fnv1a(exe[2048:]):08x}",
        "file_size": len(exe), "sha1": hashlib.sha1(exe).hexdigest()}))
    for n, serial in enumerate(SERIALS, 1):
        (game / f"files-disc{n}.json").write_text(json.dumps({"exe": {
            "name": serial, "sha1": hashlib.sha1(exe).hexdigest()}, "image": {
            "name": f"FF8DISC{n}.IMG", "lba": 826, "size": 2048,
            "sha1": hashlib.sha1(bytes([n]) * 2048).hexdigest()}}))
    return header(game)


def disc(path, number=1, *, mode=2, serial=None, image=True, exe=None,
         exe_size=None, img_size=2048, img_lba=826, cnf=True, pvd=True, offset=0,
         boot=None, bad_record=False):
    raw = bytearray((832 + offset) * 2352)

    def sector(lba, data):
        at = (lba + offset) * 2352
        raw[at + 15] = mode
        at += 16 if mode == 1 else 24
        raw[at:at + len(data)] = data

    def record(lba, size, name):
        rec = bytearray(33 + len(name) + (len(name) % 2 == 0))
        rec[0] = len(rec)
        struct.pack_into("<I", rec, 2, lba)
        struct.pack_into("<I", rec, 10, size)
        rec[32] = len(name)
        rec[33:33 + len(name)] = name
        return rec

    serial = serial or SERIALS[number - 1]
    exe = fake_exe() if exe is None else exe
    root = bytearray(2048)
    config = (boot or f"BOOT = cdrom:\\{serial};1\r\n").encode()
    records = (record(21, len(config), b"SYSTEM.CNF;1") if cnf else b"")
    records += record(30, len(exe) if exe_size is None else exe_size, (serial + ";1").encode())
    if image:
        records += record(img_lba, img_size, f"FF8DISC{number}.IMG;1".encode())
    root[:len(records)] = records
    if bad_record:
        root[0] = 33
    volume = bytearray(2048)
    if pvd:
        volume[0:7] = b"\x01CD001\x01"
    struct.pack_into("<I", volume, 158, 20)
    struct.pack_into("<I", volume, 166, 2048)
    sector(16, volume)
    sector(20, root)
    sector(21, config)
    for i in range(0, len(exe), 2048):
        sector(30 + i // 2048, exe[i:i + 2048])
    sector(img_lba, bytes([number]) * min(img_size, 2048))
    path.write_bytes(raw)
    return path


@pytest.fixture(scope="module")
def identity_binary(tmp_path_factory):
    tmp = tmp_path_factory.mktemp("disc-identity")
    (tmp / "disc_identity.h").write_text(metadata(tmp / "game"))
    binary = tmp / "identify"
    subprocess.run(["cc", "-std=c99", "-O2", "-Wall", "-Wextra", "-Wpedantic", "-Werror",
                    "-ffunction-sections", "-fdata-sections", "-Wl,--gc-sections",
                    "-Iruntime", "-Ibackends/host", f"-I{tmp}", "tests/disc_id_harness.c",
                    "backends/host/disc_id.c", "backends/host/disc_iso.c", "runtime/cpu.c",
                    "-o", str(binary)], cwd=ROOT, check=True, capture_output=True)
    return binary


def identify(binary, path, action="quick", *paths):
    return subprocess.run([str(binary), action, str(path), *map(str, paths)],
                          text=True, capture_output=True, timeout=20)


@pytest.mark.parametrize("number", range(1, 5))
@pytest.mark.parametrize("mode", [1, 2])
def test_disc_identity_and_full_hash(identity_binary, tmp_path, number, mode):
    path = disc(tmp_path / "game.bin", number, mode=mode)
    for action in ("quick", "full"):
        result = identify(identity_binary, path, action)
        assert result.returncode == 0 and result.stdout == f"disc={number}\n"
    assert identify(identity_binary, path, "exe").stdout == "EXE verified\n"


def test_cue_offset_and_order(identity_binary, tmp_path):
    paths = [disc(tmp_path / f"disc{n}.bin", n, offset=150) for n in (4, 2, 1)]
    cues = []
    for path in paths:
        cue = path.with_suffix(".cue")
        cue.write_text(f'FILE "{path.name}" BINARY\n TRACK 01 MODE2/2352\n INDEX 01 00:02:00\n')
        cues.append(cue)
    result = identify(identity_binary, cues[0], "assign", *cues[1:])
    assert result.returncode == 0
    assert result.stdout.splitlines() == [f"1={cues[2]}", f"2={cues[1]}", "3=", f"4={cues[0]}"]
    result = identify(identity_binary, cues[0], "assign", cues[0])
    assert result.returncode == 2 and result.stderr == "Two images for FF8 USA disc 4\n"


@pytest.mark.parametrize("changes,message", [
    ({"pvd": False}, "not a PlayStation disc"),
    ({"cnf": False}, "not a PlayStation disc"),
    ({"boot": "BOOT2 = cdrom:\\SLUS_008.92;1"}, "not a PlayStation disc"),
    ({"boot": "BOOT = cdrom:\\DIR\\SLUS_008.92;1"}, "Another game"),
    ({"bad_record": True}, "not a PlayStation disc"),
    ({"serial": "SCUS_123.45", "image": False}, "Another game: SCUS_123.45"),
    ({"serial": "SLES_020.80"}, "Only the USA release"),
    ({"exe_size": 2048}, "EXE size check failed"),
    ({"exe": b"broken!!" + fake_exe()[8:]}, "EXE header check failed"),
    ({"exe": fake_exe()[:2048] + bytes(2048)}, "EXE FNV-1a check failed"),
    ({"exe": fake_exe()[:100] + b"!" + fake_exe()[101:]}, "EXE SHA-1 check failed"),
    ({"image": False}, "missing or truncated"),
    ({"img_size": 4096}, "IMG size check failed"),
    ({"img_lba": 825}, "IMG LBA check failed"),
])
def test_identity_errors(identity_binary, tmp_path, changes, message):
    result = identify(identity_binary, disc(tmp_path / "game.bin", **changes))
    assert result.returncode == 2 and message in result.stderr
    assert len(result.stderr.splitlines()) == 1


def test_full_hash_is_opt_in(identity_binary, tmp_path):
    path = disc(tmp_path / "game.bin")
    with path.open("r+b") as f:
        f.seek(826 * 2352 + 24)
        f.write(b"!")
    assert identify(identity_binary, path).returncode == 0
    result = identify(identity_binary, path, "full")
    assert result.returncode == 2 and "IMG SHA-1 check failed" in result.stderr


@pytest.mark.parametrize("suffix", [".iso", ".chd", ".pbp", ".bin"])
def test_unsupported_formats(identity_binary, tmp_path, suffix):
    path = tmp_path / ("bad" + suffix)
    path.write_bytes(bytes(2048))
    result = identify(identity_binary, path)
    assert result.returncode == 2
    assert "single-track .cue with MODE2/2352 or MODE1/2352, or raw 2352-byte .bin" in result.stderr


@pytest.mark.parametrize("cue", [
    'FILE "x.bin" BINARY\nTRACK 01 MODE1/2048\nINDEX 01 00:00:00\n',
    'FILE "x.bin" BINARY\nTRACK 01 MODE2/2352\nTRACK 02 AUDIO\nINDEX 01 00:00:00\n',
    'FILE "x.bin" BINARY\nTRACK 01 MODE2/2352\nINDEX 01 00:60:00\n',
    'FILE "x.bin" BINARY\nTRACK 01 MODE2/2352\n',
    'FILE "x.bin" WAVE\nTRACK 01 MODE2/2352\nINDEX 01 00:00:00\n',
    'FILE "missing.bin" BINARY\nTRACK 01 MODE2/2352\nINDEX 01 00:00:00\n',
])
def test_cue_errors(identity_binary, tmp_path, cue):
    disc(tmp_path / "x.bin")
    path = tmp_path / "bad.cue"
    path.write_text(cue)
    result = identify(identity_binary, path)
    assert result.returncode == 2 and "supported:" in result.stderr
    assert len(result.stderr.splitlines()) == 1


def test_unreadable(identity_binary, tmp_path):
    result = identify(identity_binary, tmp_path / "absent.bin")
    assert result.returncode == 2 and "No such file" in result.stderr and "supported:" in result.stderr


@pytest.mark.parametrize("option", ["FF8_RECORD_GPU", "FF8_SPU_TRACE"])
def test_player_rejects_recording_builds(tmp_path, option):
    result = subprocess.run(["cmake", "-S", str(ROOT), "-B", str(tmp_path), "-DFF8_PLAYER=ON",
                             f"-D{option}=ON"], capture_output=True, text=True)
    assert result.returncode != 0 and "The player cannot include" in result.stderr


@pytest.fixture(scope="module")
def synthetic_player(tmp_path_factory):
    tmp = tmp_path_factory.mktemp("synthetic-player")
    build = tmp / "host"
    # Replace only the configure-time facts in this private test build. There is
    # no runtime metadata override in the distributable executable.
    subprocess.run(["cmake", "-S", str(ROOT), "-B", str(build), "-DFF8_PLAYER=ON",
                    "-DCMAKE_BUILD_TYPE=Release",
                    "-DFF8_RCHEEVOS=OFF", "-DFF8_STATE_BRIDGE=OFF"],
                   check=True, capture_output=True)
    (build / "disc_identity.h").write_text(metadata(tmp / "game"))
    subprocess.run(["cmake", "--build", str(build), "-j4"], check=True, capture_output=True, timeout=180)
    return build / "lionheart"


def play(binary, tmp, *args, automatic=False, **env):
    base = [str(binary), "--headless"]
    if not automatic and "--config" not in args:
        base += ["--config", os.devnull]
    return subprocess.run([*base, *map(str, args)], cwd=tmp, capture_output=True, text=True, timeout=30,
                          env={**os.environ, "HOME": str(tmp), "XDG_CONFIG_HOME": str(tmp / "config"),
                               "XDG_DATA_HOME": str(tmp / "data"), "SDL_VIDEODRIVER": "dummy",
                               "SDL_AUDIODRIVER": "dummy", **env})


RUN = ["--headless", "--fast", "--mute", "--max-vblanks", "2", "--log", os.devnull]


def test_no_arguments(synthetic_player, tmp_path):
    result = play(synthetic_player, tmp_path, automatic=True)
    assert result.returncode == 2
    assert result.stderr == (f"Provide FF8 USA disc 1 with --disc PATH or disc1=PATH in "
                             f"{tmp_path}/config/lionheart/lionheart.ini\n")
    assert (tmp_path / "data/lionheart/logs/lionheart.log").exists()


def test_disc_boot_default_card_and_states(synthetic_player, tmp_path):
    image = disc(tmp_path / "game.bin")
    state = tmp_path / "state.bin"
    result = play(synthetic_player, tmp_path, "--disc", image, *RUN, "--dump-state", state,
                  "--save-at", "1", "--save-state", tmp_path / "saved.ff8s")
    assert result.returncode == 0, result.stderr
    assert "VBlanks=2 " in result.stdout and "Stop: VBlank limit reached" in result.stdout
    card = tmp_path / "data/lionheart/cards/card1.mcr"
    assert card.stat().st_size == 131072 and card.read_bytes()[:2] == b"MC"
    assert not card.with_name("card2.mcr").exists()
    assert (tmp_path / "data/lionheart/states").is_dir()
    # FF8STATE1 followed by 135 CPU words, then RAM.
    dump = state.read_bytes()
    assert dump[9 + 135*4 + 0x10000:9 + 135*4 + 0x10800] == fake_exe()[2048:]
    content = card.read_bytes()
    again = play(synthetic_player, tmp_path, "--disc", image, *RUN)
    assert again.returncode == 0 and card.read_bytes() == content


def test_explicit_exe_matches_disc_boot(synthetic_player, tmp_path):
    image = disc(tmp_path / "game.bin")
    exe = tmp_path / "game.exe"
    exe.write_bytes(fake_exe())
    for label, args in [("disc", []), ("file", [str(exe)])]:
        result = play(synthetic_player, tmp_path, *args, "--disc", image, *RUN,
                      "--dump-state", tmp_path / label)
        assert result.returncode == 0, result.stderr
    assert (tmp_path / "disc").read_bytes() == (tmp_path / "file").read_bytes()


def test_ini_relative_paths_cli_and_order(synthetic_player, tmp_path):
    cfg = tmp_path / "settings"
    cfg.mkdir()
    images = [disc(cfg / f"{n}.bin", n) for n in range(1, 5)]
    ini = cfg / "lionheart.ini"
    ini.write_text("disc1=4.bin\ndisc2=2.bin\ndisc3=1.bin\ndisc4=3.bin\ncard1=one.mcr\ncard2=two.mcr\n")
    result = play(synthetic_player, tmp_path, "--config", ini, *RUN)
    assert result.returncode == 0, result.stderr
    assert (cfg / "one.mcr").read_bytes()[:2] == (cfg / "two.mcr").read_bytes()[:2] == b"MC"
    ini.write_text("disc1=absent.bin\ncard1=absent/one.mcr\ncard2=\n")
    result = play(synthetic_player, tmp_path, "--config", ini, "--disc", images[0],
                  "--card1", tmp_path / "cli.mcr", *RUN)
    assert result.returncode == 0 and (tmp_path / "cli.mcr").exists(), result.stderr


def test_portable(synthetic_player, tmp_path):
    binary = tmp_path / "ff8"
    shutil.copyfile(synthetic_player, binary)
    binary.chmod(0o755)
    (tmp_path / "portable").touch()
    result = play(binary, tmp_path, automatic=True)
    assert result.returncode == 2 and str(tmp_path / "lionheart.ini") in result.stderr
    disc(tmp_path / "disc.bin")
    (tmp_path / "lionheart.ini").write_text("disc1=disc.bin\n")
    other = tmp_path / "other"
    other.mkdir()
    result = play(binary, other, *RUN, automatic=True)
    assert result.returncode == 0, result.stderr
    assert (tmp_path / "cards/card1.mcr").read_bytes()[:2] == b"MC"
    assert not (other / "data").exists()


def test_home_fallback_and_missing_disc(synthetic_player, tmp_path):
    path = disc(tmp_path / "two.bin", 2)
    result = play(synthetic_player, tmp_path, "--disc", path, *RUN, XDG_DATA_HOME="")
    assert result.returncode == 2 and "Provide FF8 USA disc 1" in result.stderr
    path = disc(tmp_path / "one.bin")
    result = play(synthetic_player, tmp_path, "--disc", path, *RUN, XDG_DATA_HOME="")
    assert result.returncode == 0, result.stderr
    assert (tmp_path / ".local/share/lionheart/cards/card1.mcr").exists()


def test_missing_swap_is_not_fatal(synthetic_player, tmp_path):
    path = disc(tmp_path / "one.bin")
    script = tmp_path / "input.txt"
    script.write_text("1 DISC2\n2 -\n")
    result = play(synthetic_player, tmp_path, "--disc", path, *RUN, "--input-script", script)
    assert result.returncode == 0 and "Disc 2 missing" in result.stderr


def test_player_full_verification_cli_and_ini(synthetic_player, tmp_path):
    path = disc(tmp_path / "one.bin")
    with path.open("r+b") as file:
        file.seek(826 * 2352 + 24)
        file.write(b"!")
    assert play(synthetic_player, tmp_path, "--disc", path, *RUN).returncode == 0
    result = play(synthetic_player, tmp_path, "--disc", path, "--verify-discs", "full", *RUN)
    assert result.returncode == 2 and "IMG SHA-1 check failed" in result.stderr
    assert len(result.stderr.splitlines()) == 1
    ini = tmp_path / "full.ini"
    ini.write_text("disc1=one.bin\nverify-discs=full\n")
    result = play(synthetic_player, tmp_path, "--config", ini, *RUN)
    assert result.returncode == 2 and "IMG SHA-1 check failed" in result.stderr
    result = play(synthetic_player, tmp_path, "--config", ini, "--verify-discs", "quick", *RUN)
    assert result.returncode == 0, result.stderr


@pytest.fixture(scope="module")
def real_player(tmp_path_factory):
    supplied = os.environ.get("FF8_PLAYER_HOST")
    if supplied:
        return Path(supplied).resolve()
    build = tmp_path_factory.mktemp("release-player")
    subprocess.run(["cmake", "-S", str(ROOT), "-B", str(build), "-DFF8_PLAYER=ON",
                    "-DCMAKE_BUILD_TYPE=Release", "-DCMAKE_EXPORT_COMPILE_COMMANDS=ON"],
                   check=True, capture_output=True)
    subprocess.run(["cmake", "--build", str(build), "-j4"], check=True, capture_output=True, timeout=300)
    return build / "lionheart"


def test_player_compiles_no_generated_catalog(real_player):
    build = real_player.parent
    units = json.loads((build / "compile_commands.json").read_text())
    assert units and any(Path(u["file"]).name == "player_catalog.c" for u in units)
    assert not any(any("generated" in part.lower() for part in Path(u["file"]).parent.parts) or
                   re.match(r"(?:functions_|overlay_|dispatch_table|interpreter_table)", Path(u["file"]).name)
                   for u in units)
    cache = (build / "CMakeCache.txt").read_text()
    for option in ["FF8_PLAYER", "FF8_VULKAN", "FF8_PGXP", "FF8_SAVESTATES", "FF8_MOVIES",
                   "FF8_RCHEEVOS", "FF8_STATE_BRIDGE"]:
        assert f"{option}:BOOL=ON" in cache
    assert "FF8_STATS:BOOL=OFF" in cache
    assert not any(Path(u["file"]).name in ("gpu_record.c", "spu_trace.c") for u in units)


def test_exe_identity_equals_recompiler(tmp_path):
    exe = owned_path("SLUS_008.92")
    if not exe.is_file():
        pytest.skip("owned game EXE is absent")
    facts = json.loads((ROOT / "games/ff8-usa/exe-identity.json").read_text())
    image = Image.read(exe)
    emit(image, {}, tmp_path)
    dispatch = (tmp_path / "dispatch_table.c").read_text()
    assert f"generated_hash = 0x{facts['fnv1a']}u" in dispatch
    assert f"generated_base = 0x{facts['base']:08x}u, generated_size = {facts['size']}" in dispatch
    assert facts["file_size"] == exe.stat().st_size
    assert facts["sha1"] == hashlib.sha1(exe.read_bytes()).hexdigest()


def test_player_contains_no_guest_text(real_player):
    exe = owned_path("SLUS_008.92")
    if not exe.is_file():
        pytest.skip("owned game EXE is absent")
    assert check(real_player.read_bytes(), Image.read(exe).data)["passed"]


def test_legal_check_detects_windows_and_ignores_low_entropy():
    text = random.Random(14).randbytes(130)
    with pytest.raises(ValueError, match="offset 13"):
        check(bytes(13) + text[17:81] + bytes(20), text)
    assert check(bytes(200), bytes(200))["unique_guest_windows"] == 0


@pytest.mark.parametrize("kind", ["fault", "limit"])
def test_player_stop_log_and_status(synthetic_player, tmp_path, kind):
    image = disc(tmp_path / "game.bin")
    args = ["--max-instructions", "16"] if kind == "limit" else ["--stall", "4"]
    result = play(synthetic_player, tmp_path, "--disc", image, "--fast", "--mute", *args)
    assert result.returncode == (0 if kind == "limit" else 1), result.stderr
    summary = result.stdout.splitlines()[:2]
    assert summary[0].startswith("Stop: " + ("instruction budget exhausted" if kind == "limit" else "stall:"))
    assert re.fullmatch(r"Last PC: [0-9a-f]{8}", summary[1])
    log = (tmp_path / "data/lionheart/logs/lionheart.log").read_text()
    if kind == "fault":
        assert "\n".join(summary) + "\n" in log
        assert "\n".join(summary) + "\n" in result.stderr
    else:
        assert "Stop:" not in log
