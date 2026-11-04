"""Play decoder memo and repeated-block spans match independent scalar output."""
from pathlib import Path
import subprocess
import pytest

@pytest.mark.parametrize("fixture", ["spu_decode_memo_harness", "spu_decode_loop_harness"])
def test_decode_memo(tmp_path, fixture):
    root = Path(__file__).resolve().parents[1]
    binary = tmp_path / fixture
    subprocess.run(["cc", "-std=c99", "-O2", "-Wall", "-Wextra", "-Werror", str(root / "tests" / (fixture + ".c")), "-o", str(binary)], check=True)
    subprocess.run([binary], check=True, timeout=90)
