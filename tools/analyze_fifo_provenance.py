"""Analyze FIFO/store observations without certifying temporal identities.

Compare a provenance CSV and a GPU command capture in the supplied directory.
Equal values do not prove ownership or complete write generations.
"""
import argparse
from collections import Counter, defaultdict, deque
import csv
import ctypes as C
import json
from pathlib import Path
import struct
import subprocess

ROOT = Path(__file__).resolve().parents[1]


def save(path, data):
    path.write_text(json.dumps(data, indent=2) + "\n")


def analyze(args):
    folder = args.output
    projections, packets = {}, {}
    counts, bins, sources, examples = Counter(), defaultdict(Counter), defaultdict(set), []
    with (folder / "provenance.csv").open() as stream:
        for row in csv.DictReader(stream):
            kind = row.pop("kind")
            r = {k: int(v) for k, v in row.items()}
            counts[kind] += 1
            if kind == "P":
                projections[r["event"]] = r
                if r["source"]:
                    sources[r["source"]].add(r["revision"])
            elif kind == "D":
                packets[r["event"]] = r
    library = folder / "parser.so"
    subprocess.run(["cc", "-std=c99", "-O2", "-Wall", "-Wextra", "-Wpedantic", "-Werror",
                    "-fPIC", "-shared", "-Iruntime", "tools/frame_provenance_gpu.c",
                    "runtime/gpu_raster_soft.c", "-o", str(library)], cwd=ROOT, check=True)
    lib = C.CDLL(str(library))
    lib.gpu_create.restype = C.c_void_p
    for name in ("gpu_gp1", "gpu_read", "gpu_vblank", "frame_trace_free"):
        getattr(lib, name).argtypes = [C.c_void_p] + ([C.c_uint32] if name == "gpu_gp1" else [])
    lib.frame_trace_restore.argtypes = [C.c_void_p, C.c_void_p]
    lib.frame_trace_word.argtypes = [C.c_void_p, C.c_uint32, C.POINTER(C.c_uint32)]
    gpu, out = lib.gpu_create(), (C.c_uint32 * 12)()
    cache, frozen, events = {}, {}, deque(maxlen=12)
    event = frame = 0
    def window(vb):
        if vb < 2025: return "world"
        if vb < 2400: return "transition-entry"
        if vb < 2700: return "battle-2400-2699"
        return "battle-2700-end"
    try:
        with (folder / "capture.gpu").open("rb") as stream:
            assert stream.read(8) == b"FF8GPUR2"
            while header := stream.read(8):
                assert len(header) == 8
                kind, value = struct.unpack("<II", header)
                if kind == 5:
                    assert value == lib.frame_trace_size()
                    data = stream.read(value)
                    assert len(data) == value
                    lib.frame_trace_restore(gpu, C.create_string_buffer(data))
                elif kind == 6:
                    frame = value
                elif kind == 7:
                    x, y, z = struct.unpack("<qqI", stream.read(20))
                    ix, iy = C.c_int16(value).value, C.c_int16(value >> 16).value
                    valid = (0 < z <= 65535 and -1024 < ix < 1023 and -1024 < iy < 1023
                             and ix * 65536 <= x < (ix + 1) * 65536
                             and iy * 65536 <= y < (iy + 1) * 65536)
                    cache[value] = (frame, projections.get(event), valid)
                elif kind == 2:
                    lib.gpu_vblank(gpu); frame += 1; frozen.clear()
                elif kind == 1:
                    lib.gpu_gp1(gpu, value)
                elif kind == 3:
                    lib.gpu_read(gpu)
                elif kind == 0:
                    events.append(event)
                    n = lib.frame_trace_word(gpu, value, out)
                    if n and 0x20 <= out[0] >> 24 < 0x40:
                        op, at, known = out[0] >> 24, 1, 0
                        b = bins[window(frame)]
                        b["polygons"] += 1
                        for corner in range(4 if op & 8 else 3):
                            if corner and op & 16: at += 1
                            key, e = out[at], list(events)[-n + at]
                            direct = packets.get(e)
                            b["vertices"] += 1
                            if direct and direct["key"] == key:
                                b["fifo_store_candidates"] += 1; known += 1
                                b["candidates_with_source"] += bool(direct["source"])
                            if key not in frozen:
                                sample = cache.get(key)
                                frozen[key] = sample[1] if sample and sample[2] and 0 <= frame - sample[0] <= 4 else None
                            chosen = frozen[key]
                            if direct and chosen and direct["key"] == key:
                                b["comparable_sxy_candidates"] += 1
                                if direct["projection"] != chosen["projection"]:
                                    b["sxy_serial_disagreements"] += 1
                                    if direct["source"] and chosen["source"] and direct["source"] != chosen["source"]:
                                        b["sxy_source_disagreements"] += 1
                                        if len(examples) < 24:
                                            examples.append(dict(frame=frame, event=e, fifo=direct, sxy=chosen))
                            at += 1 + bool(op & 4)
                        b["all_corners_candidates"] += known == (4 if op & 8 else 3)
                        b["certified_temporal_pairs"] += 0
                elif kind not in (4, 8, 9):
                    raise ValueError(f"unknown event {kind}")
                event += 1
    finally:
        lib.frame_trace_free(gpu)
    result = dict(verdict="NO-GO", trace_counts=counts, windows=bins,
                  known_staging_addresses=len(sources),
                  changed_staging_addresses=sum(len(v) > 1 for v in sources.values()),
                  max_observed_revisions=max(map(len, sources.values()), default=0),
                  disagreements=examples,
                  limitation="FIFO/store candidates lack complete write generations and model/pass/vertex identity; zero certified pairs. Windows are route labels, not semantic classifications.")
    save(folder / "audit.json", result)
    print(json.dumps({k: v for k, v in result.items() if k != "disagreements"}, indent=2))


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, required=True,
                        help="directory containing provenance.csv and capture.gpu")
    analyze(parser.parse_args())
