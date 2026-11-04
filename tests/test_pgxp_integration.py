"""runtime projection publication on exact, play and generic GTE paths."""
from pathlib import Path
import subprocess

import pytest

ROOT = Path(__file__).resolve().parents[1]


@pytest.mark.parametrize("mode", [[], ["-DFF8_PLAY"], ["-DFF8_GTE_REFERENCE"]],
                         ids=["exact", "play", "generic"])
def test_projection_hook_preserves_gte_and_publishes_each_fifo_push(tmp_path, mode):
    states = []
    for enabled in (False, True):
        binary = tmp_path / str(enabled)
        subprocess.run(["cc", "-std=c99", "-O2", "-Wall", "-Wextra", "-Werror",
                        *mode, *(["-DFF8_PGXP"] if enabled else []), "-Iruntime",
                        "tests/gte_pgxp_hook_harness.c", "runtime/gte.c", "-o", str(binary)],
                       cwd=ROOT, check=True)
        states.append(subprocess.check_output([binary]))
    assert states[0] == states[1]
