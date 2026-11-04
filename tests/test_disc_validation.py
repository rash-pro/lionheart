"""host review fixes use synthetic media and the unchanged runtime CUE parser."""
import os
from pathlib import Path
import subprocess

import pytest

from test_host_player import ROOT, disc, identify, identity_binary, metadata


@pytest.fixture(scope="module")
def runtime_disc_binary(tmp_path_factory):
    tmp = tmp_path_factory.mktemp("host-runtime-disc")
    (tmp / "disc_identity.h").write_text(metadata(tmp / "game"))
    binary = tmp / "runtime-identify"
    subprocess.run([
        "cc", "-std=c99", "-O2", "-Wall", "-Wextra", "-Wpedantic", "-Werror",
        "-DFF8_DISC_RUNTIME_TEST", "-DFF8_DISC_ALLOC_TEST", "-ffunction-sections", "-fdata-sections", "-Wl,--gc-sections",
        "-Wl,--wrap=malloc", "-Iruntime", "-Ibackends/host", f"-I{tmp}",
        "tests/disc_id_harness.c", "backends/host/disc_id.c", "backends/host/disc_iso.c",
        "runtime/cpu.c", "runtime/cdrom.c", "-o", str(binary),
    ], cwd=ROOT, check=True, capture_output=True)
    return binary


@pytest.mark.parametrize("suffix", [".cue", ".CUE", ".CuE"])
@pytest.mark.parametrize("bom", [False, True])
def test_cue_suffix_and_bom_reaches_real_runtime(runtime_disc_binary, tmp_path, suffix, bom):
    image = disc(tmp_path / "game.bin", offset=150)
    cue = tmp_path / ("game" + suffix)
    original = ((b"\xef\xbb\xbf" if bom else b"") +
                f'FILE "{image.name}" BINARY\n TRACK 01 MODE2/2352\n INDEX 01 00:02:00\n'.encode())
    cue.write_bytes(original)
    result = identify(runtime_disc_binary, cue)
    if suffix == ".CuE":
        assert result.returncode == 2 and "unsupported image" in result.stderr
    else:
        assert result.returncode == 0 and result.stdout == "disc=1\n", result.stderr
        env = {**os.environ, "TMPDIR": str(tmp_path)}
        result = subprocess.run([str(runtime_disc_binary), "runtime", str(cue)],
                                text=True, capture_output=True, env=env, timeout=20)
        assert result.returncode == 0, result.stderr
        assert result.stdout == f"runtime offset=150 sectors=832 temporary={int(bom)}\n"
        assert not list(tmp_path.glob("lionheart-cue-*"))
    assert cue.read_bytes() == original


@pytest.mark.parametrize("boot", ["slus_008.92", "SlUs_008.92"])
def test_lowercase_boot_matches_root_file(identity_binary, tmp_path, boot):
    image = disc(tmp_path / "game.bin", boot=f"BOOT = cdrom:\\{boot};1\r\n")
    result = identify(identity_binary, image)
    assert result.returncode == 0 and result.stdout == "disc=1\n", result.stderr
    assert identify(identity_binary, image, "exe").returncode == 0


def test_lowercase_iso_root_names(identity_binary, tmp_path):
    image = disc(tmp_path / "game.bin", serial="slus_008.92")
    data = image.read_bytes().replace(b"SYSTEM.CNF;1", b"system.cnf;1")
    image.write_bytes(data)
    result = identify(identity_binary, image)
    assert result.returncode == 0 and result.stdout == "disc=1\n", result.stderr


@pytest.mark.parametrize("separator", ["\\", "/"])
def test_subdirectory_boot_is_another_game(identity_binary, tmp_path, separator):
    image = disc(tmp_path / "game.bin", boot=f"BOOT = cdrom:\\DIR{separator}SLUS_008.92;1\r\n")
    result = identify(identity_binary, image)
    assert result.returncode == 2
    assert result.stderr == "Another game: DIR?SLUS_008.92\n"


@pytest.mark.parametrize("serial,expected", [
    ("BAD\x1b[31m_%é", "BAD??31m_???"),
    ("ab-CD_09.xyz", "ab-CD_09.xyz"),
])
def test_disc_derived_serial_diagnostics_are_sanitized(identity_binary, tmp_path, serial, expected):
    image = disc(tmp_path / "game.bin", serial=serial, image=False)
    result = identify(identity_binary, image)
    assert result.returncode == 2 and result.stderr == f"Another game: {expected}\n"


def test_exe_diagnostic_sanitizes_boot_name(identity_binary, tmp_path):
    image = disc(tmp_path / "game.bin", boot="BOOT = cdrom:\\BAD\x1b[31m;1\r\n")
    result = identify(identity_binary, image, "exe")
    assert result.returncode == 2 and result.stderr == "FF8 USA EXE missing (BAD??31m)\n"


def test_missing_bin_names_bin_instead_of_cue(identity_binary, tmp_path):
    cue = tmp_path / "game.cue"
    cue.write_text('FILE "lost.bin" BINARY\n TRACK 01 MODE2/2352\n INDEX 01 00:00:00\n')
    result = identify(identity_binary, cue)
    assert result.returncode == 2
    assert result.stderr.startswith(f"{tmp_path / 'lost.bin'}: No such file")
    assert str(cue) not in result.stderr


def test_missing_exe_reports_missing(identity_binary, tmp_path):
    image = disc(tmp_path / "game.bin", serial="OTHER", boot="BOOT = cdrom:\\SLUS_008.92;1\r\n")
    result = identify(identity_binary, image)
    assert result.returncode == 2 and result.stderr == "FF8 USA EXE missing (SLUS_008.92)\n"


def test_exe_allocation_failure_reports_out_of_memory(runtime_disc_binary, tmp_path):
    image = disc(tmp_path / "game.bin")
    result = subprocess.run([str(runtime_disc_binary), "quick", str(image)], text=True,
                            capture_output=True, env={**os.environ, "FF8_TEST_FAIL_ALLOC": "1"}, timeout=20)
    assert result.returncode == 2 and result.stderr == "Out of memory reading EXE SLUS_008.92\n"
