"""Cached loop proofs expire on every guest operation that can invalidate them."""
import os
from pathlib import Path
import subprocess


def test_loop_cache_invalidation(tmp_path):
    root = Path(__file__).resolve().parents[1]
    binary = tmp_path / 'loops'
    flags = ['-fsanitize=address,undefined', '-fno-omit-frame-pointer'] if os.environ.get('FF8_TEST_SANITIZE') else []
    subprocess.run(['cc', '-std=c99', '-O2', '-Wall', '-Wextra', '-Werror', *flags,
                    str(root / 'tests/spu_loop_cache_harness.c'), '-o', str(binary)], check=True)
    subprocess.run([binary], check=True, timeout=60)
