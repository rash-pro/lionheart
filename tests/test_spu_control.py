"""Exact observable state across bulk/control DSP and dependency changes."""
import os
from pathlib import Path
import subprocess
import pytest


@pytest.fixture(scope='module', params=[False, True], ids=['exact', 'play'])
def control_binary(tmp_path_factory, request):
    root = Path(__file__).resolve().parents[1]
    binary = tmp_path_factory.mktemp('spu-control') / 'spu-control'
    flags = ['-fsanitize=address,undefined', '-fno-omit-frame-pointer'] if os.environ.get('FF8_TEST_SANITIZE') else []
    if request.param:
        flags += ['-DFF8_PLAY']
    subprocess.run(['cc', '-std=c99', '-O2', '-Wall', '-Wextra', '-Werror', *flags,
                    str(root / 'tests/spu_control_harness.c'), '-o', str(binary)], check=True)
    return binary


def test_control_matches_full_model(control_binary):
    subprocess.run([control_binary], check=True, timeout=90)


@pytest.mark.parametrize('trailing_bytes', range(12))
def test_replay_rejects_incomplete_or_empty_trace(control_binary, tmp_path, trailing_bytes):
    trace = tmp_path / 'incomplete.bin'
    trace.write_bytes(b'FF8SPUT1' + b'\x01' * trailing_bytes)
    result = subprocess.run([control_binary, trace], capture_output=True)
    assert result.returncode == 2
    assert b'PASS' not in result.stdout
