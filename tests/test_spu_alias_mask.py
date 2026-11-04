"""One alias scan preserves the prior per-voice source proof."""
from pathlib import Path
import subprocess

def test_alias_mask(tmp_path):
    root = Path(__file__).resolve().parents[1]
    binary = tmp_path / "alias"
    subprocess.run(["cc", "-std=c99", "-O2", "-Wall", "-Wextra", "-Werror", str(root / "tests/spu_alias_mask_harness.c"), "-o", str(binary)], check=True)
    subprocess.run([binary], check=True, timeout=90)
