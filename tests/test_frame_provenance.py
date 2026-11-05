"""The diagnostic must expose ambiguity, never promote value equality to identity."""
import csv
import importlib.util
import json
import os
from pathlib import Path
import subprocess
import struct
from types import SimpleNamespace

import pytest

ROOT = Path(__file__).resolve().parents[1]


@pytest.fixture(scope="module")
def probe(tmp_path_factory):
    binary = tmp_path_factory.mktemp("frame-provenance") / "probe"
    subprocess.run([
        "cc", "-std=c99", "-O2", "-Wall", "-Wextra", "-Wpedantic", "-Werror",
        "-fsanitize=undefined", "-fno-sanitize-recover=all", "-DFF8_PGXP",
        "-Iruntime", "-Ibackends/host", "tests/frame_provenance_harness.c",
        "backends/host/frame_provenance.c", "runtime/gte.c", "-Wl,--wrap=gte_read",
        "-o", str(binary)], cwd=ROOT, check=True, capture_output=True)
    return binary


def capture(probe, tmp_path, case):
    path = tmp_path / (case + ".csv")
    subprocess.run([str(probe), case], check=True,
                   env={**os.environ, "FF8_FRAME_PROVENANCE": str(path)})
    with path.open() as stream:
        return list(csv.DictReader(stream))


@pytest.mark.parametrize("case,serial", [("fifo", "1"), ("alias", "3")])
def test_equal_screen_coordinates_keep_distinct_fifo_observations(probe, tmp_path, case, serial):
    rows = capture(probe, tmp_path, case)
    projections = [r for r in rows if r["kind"] == "P"]
    assert len({r["key"] for r in projections}) == 1
    assert len({r["source"] for r in projections}) == 3
    assert rows[-1]["kind"] == "D" and rows[-1]["projection"] == serial


def test_changed_packet_value_rejects_old_observation(probe, tmp_path):
    rows = capture(probe, tmp_path, "overwrite")
    assert rows[-1]["kind"] == "X"
    assert not any(r["kind"] == "D" for r in rows)


def test_observed_fifo_write_clears_candidate(probe, tmp_path):
    rows = capture(probe, tmp_path, "invalidate")
    assert rows[-1]["kind"] == "S" and rows[-1]["projection"] == "0"
    assert not any(r["kind"] == "D" for r in rows)


def test_same_value_packet_reuse_is_indistinguishable_and_cannot_certify_identity(probe, tmp_path):
    ordinary = capture(probe, tmp_path, "fifo")
    overwritten = capture(probe, tmp_path, "same_value_reuse")
    # The guest executed two extra ordinary RAM stores. The observer sees
    # identical evidence: a concrete counterexample to trusting this trace.
    assert ordinary == overwritten


def test_staging_value_change_is_observed_without_claiming_all_writes(probe, tmp_path):
    rows = capture(probe, tmp_path, "revision")
    revisions = [r["revision"] for r in rows if r["kind"] == "P" and r["source"] == "8192"]
    assert revisions == ["1", "2"]


def test_audit_compares_packet_vertices_with_independent_fifo_observations(tmp_path):
    spec = importlib.util.spec_from_file_location("provenance_audit", ROOT / "tools/analyze_fifo_provenance.py")
    audit = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(audit)
    (tmp_path / "provenance.csv").write_text(
        "kind,frame,pc,address,projection,source,key,event,revision\n"
        "P,0,4096,0,2,8192,0,0,1\n"
        "D,0,4096,12288,1,8200,0,2,1\n")
    # The upload payload resembles a polygon command. It must not be counted.
    events = [(7, 0), (0, 0x200000ff), (0, 0), (0, 10), (0, 10 << 16),
              (0, 0xa0000000), (0, 0), (0, 0x00010002), (0, 0x200000ff)]
    data = bytearray(b"FF8GPUR2")
    for kind, value in events:
        data += struct.pack("<II", kind, value)
        if kind == 7:
            data += struct.pack("<qqI", 0, 0, 100)
    (tmp_path / "capture.gpu").write_bytes(data)
    audit.analyze(SimpleNamespace(output=tmp_path))
    result = json.loads((tmp_path / "audit.json").read_text())
    window = result["windows"]["world"]
    assert window["polygons"] == 1 and window["vertices"] == 3
    assert window["sxy_source_disagreements"] == 1
    assert result["disagreements"][0]["event"] == 2
    assert window["certified_temporal_pairs"] == 0
    assert result["verdict"] == "NO-GO"
