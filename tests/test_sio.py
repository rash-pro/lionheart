"""SIO0/card wire transactions and deterministic scripts; synthetic data only."""
import os
from pathlib import Path
import subprocess

import pytest

ROOT = Path(__file__).resolve().parents[1]


@pytest.fixture(scope="module")
def sio_binary(tmp_path_factory):
    output = tmp_path_factory.mktemp("sio") / "harness"
    flags = ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"] if os.environ.get("FF8_TEST_SANITIZE") else []
    subprocess.run(["cc", "-std=c99", "-O1", "-g", "-Wall", "-Wextra", "-Werror", *flags,
                    "-I", str(ROOT / "runtime"), str(ROOT / "tests/sio_harness.c"),
                    *map(str, sorted((ROOT / "runtime").glob("*.c"))), "-o", str(output)], check=True)
    return output


def run(binary, case, *args):
    subprocess.run([str(binary), case, *map(str, args)], check=True, timeout=30)


@pytest.mark.parametrize("case", ["pad", "timing", "fifo", "dualshock"])
def test_sio(sio_binary, case):
    run(sio_binary, case)


def test_card_protocol_and_persistence(sio_binary, tmp_path):
    path = tmp_path / "card.mcr"
    run(sio_binary, "card", path)
    data = path.read_bytes()
    assert len(data) == 131072
    assert data[:2] == b"MC"
    for frame in range(36):
        block = data[frame * 128:(frame + 1) * 128]
        checksum = 0
        for byte in block:
            checksum ^= byte
        assert checksum == 0
    for frame in range(1, 16):
        block = data[frame * 128:(frame + 1) * 128]
        assert block[0] == 0xA0 and block[8:10] == b"\xff\xff"
    assert data[7 * 128:8 * 128] == bytes([0xA0]) + bytes(7) + b"\xff\xff" + bytes(117) + bytes([0xA0])
    assert data[-128:] == bytes(i ^ 0x5A for i in range(128))


@pytest.mark.parametrize("size", [0, 131071, 131073, 131072 + 64])
def test_invalid_card_is_not_modified(sio_binary, tmp_path, size):
    path = tmp_path / "invalid.mcd"
    original = bytes([0xAB]) * size
    path.write_bytes(original)
    run(sio_binary, "invalid_card", path)
    assert path.read_bytes() == original


def test_existing_raw_card_is_not_reformatted(sio_binary, tmp_path):
    path = tmp_path / "dump.mcd"
    original = bytes(range(256)) * 512
    path.write_bytes(original)
    run(sio_binary, "card_existing", path)
    assert path.read_bytes() == original


def test_card_write_failure_is_reported(sio_binary, tmp_path):
    path = tmp_path / "card.mcr"
    run(sio_binary, "card_failed", path)
    assert path.read_bytes()[-128:] == b"\xff" * 128


def test_script_exact_frames_and_release(sio_binary, tmp_path):
    path = tmp_path / "input.txt"
    path.write_text("# Script overrides live port 1 input\n10 START+CROSS\n20 -\n")
    run(sio_binary, "script", path)


def test_script_disc_request_preserves_pad_state(sio_binary, tmp_path):
    path = tmp_path / "swap.txt"
    path.write_text("10 CROSS\n15 DISC2\n20 -\n")
    run(sio_binary, "disc_script", path)


@pytest.mark.parametrize("text", ["10 START\n9 -\n", "10 START\n10 CROSS\n", "-1 START\n",
                                   "1 X\n", "1 START+\n", "1 +START\n", "1 START CROSS\n",
                                   "18446744073709551616 START\n", "1 START junk\n", "1 DISC0\n", "1 DISC5\n",
                                   "1 DISC2+CROSS\n"])
def test_invalid_scripts(sio_binary, tmp_path, text):
    path = tmp_path / "input.txt"
    path.write_text(text)
    run(sio_binary, "invalid_script", path)


@pytest.mark.parametrize("render_driver", [None, "software"])
def test_sdl_events_and_virtual_controllers(tmp_path, render_driver):
    import shlex
    output = tmp_path / "host-input"
    sdl = shlex.split(subprocess.check_output(["pkg-config", "--cflags", "--libs", "sdl2"], text=True))
    flags = ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"] if os.environ.get("FF8_TEST_SANITIZE") else []
    subprocess.run(["cc", "-std=c99", "-O1", "-g", "-Wall", "-Wextra", "-Werror", *flags,
                    "-Iruntime", "-Ibackends/host", "tests/sio_host_harness.c", "backends/host/display.c", "backends/host/screenshot.c", "backends/host/osd.c",
                    "backends/host/viewport.c",
                    "runtime/gpu.c", "runtime/gpu_raster_soft.c", "-o", str(output), *sdl, "-lz"], cwd=ROOT, check=True)
    env = {**os.environ, "SDL_VIDEODRIVER": "dummy"}
    if render_driver is not None:
        env["SDL_RENDER_DRIVER"] = render_driver
    subprocess.run([str(output)], check=True, timeout=30, env=env)


def test_dualshock_sdl_wire_and_feedback(tmp_path):
    import shlex
    output = tmp_path / "dualshock"
    sdl = shlex.split(subprocess.check_output(["pkg-config", "--cflags", "--libs", "sdl2"], text=True))
    flags = ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"] if os.environ.get("FF8_TEST_SANITIZE") else []
    subprocess.run(["cc", "-std=c99", "-O1", "-g", "-Wall", "-Wextra", "-Werror", *flags,
                    "-Iruntime", "-Ibackends/host", "tests/host_pad_harness.c", "backends/host/display.c", "backends/host/screenshot.c", "backends/host/osd.c",
                    "backends/host/viewport.c",
                    "backends/host/input.c", *map(str, sorted((ROOT / "runtime").glob("*.c"))),
                    "-Wl,--wrap=SDL_GameControllerRumble,--wrap=SDL_GameControllerSetLED", "-o", str(output), *sdl, "-lz"], cwd=ROOT, check=True)
    script = tmp_path / "digital.txt"
    script.write_text("0 CROSS\n")
    for args in ([], [str(script)]):
        subprocess.run([str(output), *args], check=True, timeout=30,
                       env={**os.environ, "SDL_VIDEODRIVER": "dummy", "SDL_AUDIODRIVER": "dummy"})


def test_real_disc_probe_initializes_trigger_rest(tmp_path):
    import shlex
    output = tmp_path / "probe-triggers"
    sdl = shlex.split(subprocess.check_output(["pkg-config", "--cflags", "--libs", "sdl2"], text=True))
    flags = ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"] if os.environ.get("FF8_TEST_SANITIZE") else []
    subprocess.run(["cc", "-std=c99", "-O1", "-g", "-Wall", "-Wextra", "-Werror", *flags,
                    "-Iruntime", "-Ibackends/host", "tests/host_pad_probe_harness.c", "tests/host_pad_probe.c",
                    "backends/host/display.c", "backends/host/screenshot.c", "backends/host/osd.c", "backends/host/input.c", "backends/host/viewport.c",
                    *map(str, sorted((ROOT / "runtime").glob("*.c"))), "-o", str(output), *sdl, "-lz"], cwd=ROOT, check=True)
    subprocess.run([str(output)], check=True, timeout=30,
                   env={**os.environ, "SDL_VIDEODRIVER": "dummy", "SDL_AUDIODRIVER": "dummy"})
