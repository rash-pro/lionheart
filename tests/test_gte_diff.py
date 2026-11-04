"""The trace replayer rejects missing work and compares every raw register."""
import os
from pathlib import Path
import struct
import subprocess

import pytest

from tools.gte_trace import build_replay

ROOT = Path(__file__).resolve().parents[1]


@pytest.fixture(scope="module", params=[(), ('-DFF8_PLAY',)], ids=['exact', 'play'])
def replay(tmp_path_factory, request):
    return build_replay(tmp_path_factory.mktemp("gte-diff"), os.environ.get("CC", "cc"), extra=request.param)


def test_extreme_command_sequences(replay):
    result = subprocess.run([str(replay)], capture_output=True, text=True, check=True)
    assert "GTE SYNTHETIC PASS commands=100000" in result.stdout
    assert "GTE NCLIP PASS corners=729" in result.stdout
    assert "GTE DIFF PASS" in result.stdout


@pytest.mark.parametrize("damage", [None, "header", "empty", "truncated", "output", "flag"])
def test_recorded_outputs_and_trace_integrity(replay, tmp_path, damage):
    # NCLIP with zero coordinates leaves every register zero.
    record = [0] * 131
    record[:3] = [2400, 0x80010000, 0x4A000006]
    if damage == "output":
        record[67 + 24] = 1
    if damage == "flag":
        record[99 + 31] = 0x80000000
    data = b"FF8GTE1\0" + struct.pack("<131I", *record)
    if damage == "header":
        data = b"invalid!" + data[8:]
    elif damage == "empty":
        data = data[:8]
    elif damage == "truncated":
        data = data[:-1]
    trace = tmp_path / "trace.gte"
    trace.write_bytes(data)
    result = subprocess.run([str(replay), str(trace)], capture_output=True, text=True)
    assert result.returncode == (0 if damage is None else 2), result.stdout
    assert ("GTE TRACE PASS" if damage is None else "GTE TRACE FAIL") in result.stdout


@pytest.mark.parametrize("specialized", [False, True])
def test_trace_observer_bounds_and_complete_output(replay, tmp_path, specialized):
    source = tmp_path / "record.c"
    source.write_text('''#include "cpu.h"
#include "hw.h"
#include <stdlib.h>
int main(void) {
    cpu_t *c = calloc(1, sizeof(*c));
    struct hw *hw = calloc(1, sizeof(*hw));
    if (!c || !hw) return 2;
    c->hw = hw;
    for (hw->vblanks = 0; hw->vblanks < 4; ++hw->vblanks)
        gte_cmd(c, 6);
    free(hw); free(c); return 0;
}
''')
    if specialized:
        source.write_text(source.read_text().replace('#include "cpu.h"', '#include "cpu.h"\n#include "gte.h"')
                          .replace('gte_cmd(c, 6);', 'gte_nclip(c);'))
    from recomp.emit import GTE_COMMANDS
    symbols = ["gte_cmd", *(["gte_" + name for name in GTE_COMMANDS.values()] if specialized else [])]
    binary = tmp_path / "record"
    subprocess.run([os.environ.get("CC", "cc"), "-std=c99", "-O2", "-Wall", "-Wextra", "-Werror",
                    *(["-DFF8_GTE_SPECIALIZED_TRACE"] if specialized else []),
                    "-I", str(ROOT / "runtime"), str(source), str(ROOT / "tools/gte_trace.c"),
                    str(ROOT / "runtime/gte.c"), "-Wl," + ",".join("--wrap=" + name for name in symbols), "-o", str(binary)], check=True)
    trace = tmp_path / "observed.gte"
    env = dict(os.environ, FF8_GTE_TRACE=str(trace), FF8_GTE_TRACE_FIRST="1", FF8_GTE_TRACE_LAST="2")
    result = subprocess.run([str(binary)], env=env, capture_output=True, text=True, check=True)
    assert "records=2" in result.stdout
    result = subprocess.run([str(replay), str(trace)], capture_output=True, text=True, check=True)
    assert "records=2" in result.stdout
    result = subprocess.run([str(binary)], env=dict(env, FF8_GTE_TRACE_LIMIT="1"), capture_output=True, text=True)
    assert result.returncode == 2 and "command limit reached" in result.stderr
