"""Host settings parser: real files, defaults and CLI precedence."""
import os
from pathlib import Path
import shlex
import subprocess

import pytest

ROOT = Path(__file__).resolve().parents[1]


@pytest.fixture
def options_binary(tmp_path):
    output = tmp_path / "options"
    sdl = shlex.split(subprocess.check_output(["pkg-config", "--cflags", "--libs", "sdl2"], text=True))
    subprocess.run(["cc", "-std=c99", "-Wall", "-Wextra", "-Werror", "-Ibackends/host",
                    "tests/host_options_harness.c", "backends/host/options.c", "-o", str(output), *sdl], cwd=ROOT, check=True)
    return output


def run(binary, *args, automatic_config=False, **env):
    if not automatic_config and "--config" not in args:
        args = (*args, "--config", os.devnull)
    return subprocess.run([str(binary), "unused.exe", *args], text=True, capture_output=True,
                          env={**os.environ, "HOME": str(binary.parent), "XDG_CONFIG_HOME": str(binary.parent / "xdg"), **env})


def test_defaults_and_classification(options_binary):
    result = run(options_binary)
    assert result.returncode == 0 and result.stdout.splitlines()[:6] == [
        "0 1 0 0 0", "pad L", "rumble P", "fullscreen P", "scale-mode P", "filter P"]
    classes = dict(line.split() for line in result.stdout.splitlines()[1:])
    assert set(classes) >= {"renderer", "pgxp", "movies", "state-bridge", "achievements", "load-state", "save-at"}
    assert all(value == ("L" if key in ("pad", "savestate-cards") else "P") for key, value in classes.items())


def test_frame_interpolation_default_config_and_cli(options_binary):
    assert run(options_binary, FF8_TEST_INTERPOLATION='1').stdout.endswith('frame-interpolation=0\n')
    path = options_binary.parent / 'interpolation.ini'
    path.write_text('frame-interpolation=on\n')
    assert run(options_binary, '--config', str(path), FF8_TEST_INTERPOLATION='1').stdout.endswith('frame-interpolation=1\n')
    assert run(options_binary, '--config', str(path), '--frame-interpolation', 'off',
               FF8_TEST_INTERPOLATION='1').stdout.endswith('frame-interpolation=0\n')
    assert run(options_binary, '--frame-interpolation', 'yes').returncode == 2


def test_config_and_cli_precedence(options_binary):
    path = options_binary.parent / "custom.ini"
    path.write_text(" # settings\n pad = dualshock ; comment\nrumble=0\nfullscreen=1\nscale-mode=integer\nfilter=linear\nfuture-key=yes\n")
    result = run(options_binary, "--pad", "digital", "--config", str(path), "--windowed", "--filter", "nearest")
    assert result.returncode == 0 and result.stdout.startswith("0 0 0 1 0\n")
    assert "unknown key 'future-key' (ignored)" in result.stderr


def test_default_paths(options_binary):
    xdg = options_binary.parent / "xdg/ff8-recomp/ff8.ini"
    xdg.parent.mkdir(parents=True)
    xdg.write_text("filter=linear\n")
    assert run(options_binary, automatic_config=True).stdout.startswith("0 1 0 0 1\n")
    local = options_binary.parent / "ff8.ini"
    local.write_text("pad=dualshock\n")
    assert run(options_binary, automatic_config=True).stdout.startswith("1 1 0 0 0\n")
    local.unlink()
    home = options_binary.parent / ".config/ff8-recomp/ff8.ini"
    home.parent.mkdir(parents=True)
    home.write_text("scale-mode=stretch\n")
    assert run(options_binary, automatic_config=True, XDG_CONFIG_HOME="").stdout.startswith("0 1 0 2 0\n")


@pytest.mark.parametrize("text", ["pad=analog\n", "rumble=2\n", "fullscreen=yes\n", "filter=linear junk\n",
                                  "scale-mode=\n", "[section]\n", "=value\n", "filter=" + "a" * 512])
def test_invalid_config(options_binary, text):
    path = options_binary.parent / "bad.ini"
    path.write_text(text)
    result = run(options_binary, "--config", str(path))
    assert result.returncode == 2 and "Config " in result.stderr


@pytest.mark.parametrize("args", [["--pad"], ["--pad", "ANALOG"], ["--rumble", "-1"], ["--config"],
                                  ["--config", "missing.ini"], ["--filter", "bilinear"], ["--scale-mode", "crop"]])
def test_invalid_cli(options_binary, args):
    assert run(options_binary, *args).returncode == 2


@pytest.mark.parametrize("args,expected", [(["--fullscreen", "0"], 0), (["--fullscreen", "1"], 1),
                                         (["--fullscreen"], 1), (["--fullscreen", "1", "--fullscreen", "0"], 0)])
def test_fullscreen_cli_values(options_binary, args, expected):
    result = run(options_binary, *args)
    assert result.returncode == 0 and int(result.stdout.splitlines()[0].split()[2]) == expected


def test_explicit_empty_config_ignores_adjacent_file(options_binary):
    (options_binary.parent / "ff8.ini").write_text("pad=invalid-from-stray-config\n")
    result = run(options_binary, "--config", os.devnull)
    assert result.returncode == 0 and result.stdout.startswith("0 1 0 0 0\n")


def test_integrated_config_and_cli_precedence(options_binary):
    path = options_binary.parent / "pc.ini"
    path.write_text("renderer=vulkan\ninternal-scale=4\nmsaa=4\npgxp=perspective\nmovies=/tmp/user movies\nstate-bridge=8765\nsave-at=1900\n")
    result = run(options_binary, "--config", str(path), "--internal-scale", "2", "--pgxp", "off", FF8_TEST_OPTIONS="1")
    assert result.returncode == 0
    assert result.stdout.splitlines()[-1] == "1 2 4 0 /tmp/user movies 8765 1900"


@pytest.mark.parametrize("args", [["--internal-scale", "9"], ["--msaa", "3"], ["--save-at", "-1"],
                                  ["--state-bridge", "65536"], ["--state-bridge", "fd:-1"], ["--pgxp", "bad"]])
def test_integrated_invalid_settings(options_binary, args):
    assert run(options_binary, *args).returncode == 2


def test_card_policy_default_config_and_cli(options_binary):
    assert run(options_binary, FF8_TEST_CARDS="1").stdout.splitlines()[-1] == "cards=live"
    path = options_binary.parent / "cards.ini"
    path.write_text("savestate-cards=snapshot\n")
    assert run(options_binary, "--config", str(path), FF8_TEST_CARDS="1").stdout.splitlines()[-1] == "cards=snapshot"
    result = run(options_binary, "--savestate-cards", "live", "--config", str(path), FF8_TEST_CARDS="1")
    assert result.returncode == 0 and result.stdout.splitlines()[-1] == "cards=live"
    result = run(options_binary, "--savestate-cards", "snapshot", FF8_TEST_CARDS="1")
    assert result.returncode == 0 and result.stdout.splitlines()[-1] == "cards=snapshot"
    assert run(options_binary, "--savestate-cards", "rollback").returncode == 2
    path.write_text("savestate-cards=rollback\n")
    assert run(options_binary, "--config", str(path)).returncode == 2


