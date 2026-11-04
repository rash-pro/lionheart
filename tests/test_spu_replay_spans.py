"""Shadow replay spans preserve PCM dependencies and mutable RAM aliases."""
import pytest
import os
from pathlib import Path
import subprocess


@pytest.mark.parametrize("play", [False, True])
def test_replay_dependencies(tmp_path, play):
    root = Path(__file__).resolve().parents[1]
    binary = tmp_path / 'replay'
    flags = ['-fsanitize=address,undefined', '-fno-omit-frame-pointer'] if os.environ.get('FF8_TEST_SANITIZE') else []
    if play: flags += ['-DFF8_PLAY']
    subprocess.run(['cc', '-std=c99', '-O2', '-Wall', '-Wextra', '-Werror', *flags,
                    str(root / 'tests/spu_replay_harness.c'), '-o', str(binary)], check=True)
    subprocess.run([binary], check=True, timeout=90)
