"""Live card ownership survives load; snapshot mode preserves deterministic isolation."""
import os
from pathlib import Path
import shlex
import subprocess

import pytest

ROOT = Path(__file__).resolve().parents[1]


@pytest.fixture(scope="module")
def cards_host(tmp_path_factory):
    out = tmp_path_factory.mktemp("cards-host")
    (out / "state_build.h").write_text('#define FF8_STATE_BUILD "' + '0'*64 + '"\n#define FF8_STATE_PRECISE 1\n')
    sdl = shlex.split(subprocess.check_output(["pkg-config", "--cflags", "--libs", "sdl2"], text=True))
    sanitize = ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"] if os.environ.get("FF8_TEST_SANITIZE") else []
    binary = out / "cards"
    subprocess.run(["cc", "-std=c99", "-O1", "-g", "-Wall", "-Wextra", "-Wpedantic", "-Werror",
                    "-DFF8_STATS", "-DFF8_SAVESTATES", *sanitize, "-Iruntime", "-Ibackends/host", "-I"+str(out),
                    "tests/host_state_cards_harness.c", "backends/host/states.c", "backends/host/state_disc.c", "backends/host/disc_iso.c",
                    "backends/host/input.c", "backends/host/display.c", "backends/host/screenshot.c", "backends/host/osd.c", "backends/host/viewport.c", "backends/host/audio.c",
                    *map(str, sorted((ROOT / "runtime").glob("*.c"))), "-Wl,--wrap=host_osd_event", "-o", str(binary), *sdl, "-lz"], cwd=ROOT, check=True)
    return binary


@pytest.mark.parametrize("mode", ["live", "snapshot"])
@pytest.mark.parametrize("case", ["equal", "changed", "absent", "failed", "wire", "bios-write", "bios-load", "bios-ack", "bios-file"])
def test_restored_cards_and_guest_save(cards_host, tmp_path, mode, case):
    p = subprocess.run([str(cards_host), case, mode, str(tmp_path)], text=True, capture_output=True,
                       env={**os.environ, "SDL_VIDEODRIVER": "dummy", "SDL_AUDIODRIVER": "dummy"}, timeout=60)
    assert p.returncode == 0, p.stdout+p.stderr
    assert "Cards:" in p.stdout
    warnings = [line for line in p.stderr.splitlines() if line.startswith("Warning: live memory card")]
    assert len(warnings) == (2 if mode == "live" and case not in ("equal", "failed") else 0)
    if mode == "live":
        assert "Restored card" not in p.stdout and not list(tmp_path.glob("restored-card*"))
    elif case != "failed":
        assert len(list(tmp_path.glob("restored-card*"))) == 2
