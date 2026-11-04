"""Noise frequency changes do not inherit a countdown at the old rate."""
from pathlib import Path
import os
import subprocess

import pytest


@pytest.mark.parametrize("play", [False, True], ids=["exact", "play"])
def test_noise_clock_transitions(tmp_path, play):
    root = Path(__file__).resolve().parents[1]
    binary = tmp_path / "noise-clock"
    flags = (["-DFF8_PLAY"] if play else ["-DFF8_SAVESTATES"])
    if os.environ.get("FF8_TEST_SANITIZE"):
        flags += ["-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-O1"]
    subprocess.run(["cc", "-std=c99", "-O2", "-Wall", "-Wextra", "-Werror",
                    *flags, str(root / "tests/spu_noise_clock_harness.c"),
                    str(root / "runtime/savestate_io.c"), "-o", str(binary)], check=True)
    subprocess.run([binary], check=True, timeout=30)
