"""Optional real-disc boot regression; all disc-derived output stays in build/."""
import hashlib
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile

import pytest
from conftest import owned_path

ROOT = Path(__file__).resolve().parents[1]


@pytest.fixture(scope="module")
def real_host():
    from conftest import owned_disc, owned_path
    disc, exe = owned_disc(1), owned_path("SLUS_008.92")
    binary = ROOT / "build/dev/ff8"
    assert binary.is_file(), "run cmake -P tools/setup.cmake -- --disc1 DISC first"
    host = ROOT / "build/integration"
    host.mkdir(parents=True, exist_ok=True)
    return disc, exe, binary, host


def test_recompiled_boot_frame_hashes(real_host):
    disc, exe, binary, host = real_host
    output = Path(tempfile.mkdtemp(prefix="boot-regression-", dir=host))
    result = subprocess.run([str(binary), str(exe), "--disc", str(disc),
                             "--config", os.devnull, "--headless", "--mute", "--stall", "0", "--max-instructions", "0",
                             "--max-vblanks", "2850", "--frame-step", "30", "--log", "/dev/null",
                             "--input-script", str(ROOT / "tests/scripts/to-title.txt"),
                             "--dump-frames", str(output / "frames")],
                            cwd=ROOT, check=True, capture_output=True, text=True, timeout=180)
    (output / "summary.txt").write_text(result.stdout + result.stderr)
    assert "Stop: VBlank limit reached" in result.stdout
    assert "VBlanks=2850 " in result.stdout
    count = re.search(r"Instructions: interpreter=\d+ recompiled=(\d+)", result.stdout)
    assert count and int(count[1]) > 0, "the boot gate must execute compiled code with statistics enabled"
    expected = {
        300: "53f470202aae9839511410ccb736f4e9222908135d749594c863fe5f70550225",
        600: "26e09e018f28f65f1f73a700e605e2adb1c4c31786a6543f6cb938551ee1c374",
        750: "1c3280234cc725537df614d82f6771c1f8d4ddccee802b8f232b24f7dad7966f",
        2820: "4b847ef9e45607b9ebf9f12bc6242b124e8de7661e63fbed5fa36e6dd7e7c8bd",
    }
    for frame, digest in expected.items():
        png = output / f"frames/frame-{frame:06d}.png"
        assert hashlib.sha256(png.read_bytes()).hexdigest() == digest, str(png)


def test_recompiled_real_card_save_list(real_host):
    disc, exe, binary, host = real_host
    source = owned_path("saves/disc1-early.mcr")
    if not source.is_file():
        pytest.skip("the user's real disc1-early card is absent")
    original = hashlib.sha256(source.read_bytes()).hexdigest()
    output = Path(tempfile.mkdtemp(prefix="card-regression-", dir=host))
    card = output / "card1.mcr"
    shutil.copyfile(source, card)
    result = subprocess.run([str(binary), str(exe), "--disc", str(disc), "--config", os.devnull, "--headless", "--mute",
                             "--stall", "0", "--max-instructions", "0", "--max-vblanks", "3960",
                             "--frame-step", "60", "--frames-from", "3900", "--log", "/dev/null",
                             "--input-script", str(ROOT / "tests/scripts/load-card.txt"),
                             "--card1", str(card), "--dump-frames", str(output / "frames")],
                            cwd=ROOT, check=True, capture_output=True, text=True, timeout=240)
    (output / "summary.txt").write_text(result.stdout + result.stderr)
    assert "Stop: VBlank limit reached" in result.stdout
    count = re.search(r"Instructions: interpreter=\d+ recompiled=(\d+)", result.stdout)
    assert count and int(count[1]) > 0
    png = output / "frames/frame-003900.png"
    assert hashlib.sha256(png.read_bytes()).hexdigest() == "4687f3273bad6cc67962d52a35c647e53ee855b4b22ae0ace11be550a2e06ed8"
    assert hashlib.sha256(source.read_bytes()).hexdigest() == original
