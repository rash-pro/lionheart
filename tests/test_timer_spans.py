"""Counter batching and materialization retain timer observations and IRQ dates."""
import os
from pathlib import Path
import subprocess


def test_counter_spans_and_deadlines(tmp_path):
    root = Path(__file__).resolve().parents[1]
    binary = tmp_path / 'timers'
    flags = ['-fsanitize=address,undefined', '-fno-omit-frame-pointer'] if os.environ.get('FF8_TEST_SANITIZE') else []
    subprocess.run(['cc', '-std=c99', '-O2', '-Wall', '-Wextra', '-Werror', *flags,
                    str(root / 'tests/timer_spans_harness.c'), '-o', str(binary)], check=True)
    subprocess.run([binary], check=True, timeout=30)
