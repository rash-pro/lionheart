"""BIOS card operations share storage with SIO and complete through kernel events."""
import os
from pathlib import Path
import subprocess

import pytest

ROOT = Path(__file__).resolve().parents[1]


@pytest.fixture(scope="module")
def card_binary(tmp_path_factory):
    output = tmp_path_factory.mktemp("bios-card") / "harness"
    flags = ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"] if os.environ.get("FF8_TEST_SANITIZE") else []
    subprocess.run(["cc", "-std=c99", "-O1", "-g", "-Wall", "-Wextra", "-Wpedantic", "-Werror", *flags,
                    "-I", str(ROOT / "runtime"), str(ROOT / "tests/bios_card_harness.c"),
                    *map(str, sorted((ROOT / "runtime").glob("*.c"))), "-o", str(output)], check=True)
    return output


@pytest.mark.parametrize("case", ["missing", "storage", "load", "device_directory", "unformatted", "callbacks", "failure", "pad",
                                 "filesystem", "filesystem_async", "filesystem_failure", "filesystem_corrupt"])
def test_bios_cards(card_binary, tmp_path, case):
    path = tmp_path / "card.mcr"
    if case == "unformatted":
        path.write_bytes(bytes([0xcc]) * 131072)
    subprocess.run([str(card_binary), case, str(path)], check=True, timeout=30)
    if case == "storage":
        data = path.read_bytes()
        assert len(data) == 131072 and data[:2] == b"MC"
        assert data[-128:] == bytes(i ^ 0x5a for i in range(128))
    if case == "unformatted":
        data = path.read_bytes()
        assert data[:2] == b"MC" and data[-128:] == bytes([0xcc]) * 128
