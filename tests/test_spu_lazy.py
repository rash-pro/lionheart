"""Lazy play-mode synthesis must preserve real guest observations."""
from pathlib import Path
import subprocess


def test_lazy_observers_match_scalar_without_unobserved_replay(tmp_path):
    root = Path(__file__).resolve().parents[1]
    binary = tmp_path / 'spu-lazy'
    subprocess.run(['cc', '-std=c99', '-O2', '-Wall', '-Wextra', '-Werror', '-DFF8_PLAY',
                    str(root / 'tests/spu_lazy_harness.c'), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True, timeout=90)
