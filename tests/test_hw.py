"""Synthetic controller/kernel tests; no copyrighted game data required."""
import os
from pathlib import Path
import subprocess

import pytest

ROOT = Path(__file__).resolve().parents[1]


@pytest.fixture(scope="module")
def hw_binary(tmp_path_factory):
    output = tmp_path_factory.mktemp("hw") / "harness"
    flags = ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"] if os.environ.get("FF8_TEST_SANITIZE") else []
    subprocess.run(["cc", "-std=c99", "-O1", "-g", "-Wall", "-Wextra", "-Werror", *flags,
                    "-DFF8_STATS", "-I", str(ROOT / "runtime"), str(ROOT / "tests/hw_harness.c"),
                    *map(str, sorted((ROOT / "runtime").glob("*.c"))), "-o", str(output)], check=True)
    return output


@pytest.fixture
def raw_disc(tmp_path):
    image = tmp_path / "synthetic.bin"
    raw = bytearray()
    for sector in range(8):
        payload = bytearray((i + sector) & 255 for i in range(2352))
        payload[15] = 2
        raw.extend(payload)
    image.write_bytes(raw)
    cue = tmp_path / "synthetic.cue"
    cue.write_text('FILE "synthetic.bin" BINARY\n TRACK 01 MODE2/2352\n INDEX 01 00:00:00\n')
    return cue


@pytest.mark.parametrize("case", ["pending_sr", "masked_deadlines", "scheduler", "dma_irq_deadline", "cd", "dma", "cd_status", "no_disc", "disc_swap", "gpu_dma", "events", "kernel_ram_chains", "kernel_ram_threads", "kernel_ram_threads_syscall", "kernel_ram_threads_branch", "timers", "pal_timing", "timer_modes", "wait_event", "bios_pad", "misc"])
def test_hardware(hw_binary, raw_disc, case):
    extra = []
    if case == "disc_swap":
        replacement = raw_disc.parent / "replacement.bin"
        data = bytearray((raw_disc.parent / "synthetic.bin").read_bytes())
        data[24] = 0xa5
        replacement.write_bytes(data)
        extra = [str(replacement)]
    subprocess.run([str(hw_binary), case, str(raw_disc), *extra], check=True, timeout=30)


def test_default_stats_off_preserves_stall_progress(tmp_path):
    output = tmp_path / "harness-default"
    subprocess.run(["cc", "-std=c99", "-O2", "-Wall", "-Wextra", "-Werror",
                    "-I", str(ROOT / "runtime"), str(ROOT / "tests/hw_harness.c"),
                    *map(str, sorted((ROOT / "runtime").glob("*.c"))), "-o", str(output)], check=True)
    subprocess.run([str(output), "stats_off"], check=True, timeout=30)


