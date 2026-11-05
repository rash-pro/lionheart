"""Check temporal tag identities against independently captured GTE inputs."""
import argparse
from collections import Counter, defaultdict
import csv
import json
import mmap
from pathlib import Path
import struct

RECORD = struct.Struct("<QIIII8I8I8Q")


def signed16(value):
    return (value & 32767) - (value & 32768)


def inspect(trace, oracle, first, last, opaque_only=False):
    result = Counter()
    frames = defaultdict(dict)
    meshes = defaultdict(lambda: dict(corners=0, opcodes=set(), frames=set(), z=set()))
    examples = []
    with oracle.open("rb") as stream, mmap.mmap(stream.fileno(), 0, access=mmap.ACCESS_READ) as data:
        assert data[:16] == b"FRAMEORACLE00001"
        assert (len(data) - 16) % RECORD.size == 0
        start = RECORD.unpack_from(data, 16)[0]
        for row in csv.DictReader(trace.open()):
            if not row["kind"].startswith("C") or not first <= int(row["frame"]) < last:
                continue
            result["all_corners"] += 1
            opacity = "semitransparent" if int(row["opcode"]) & 2 else "opaque"
            result[opacity + "_corners"] += 1
            if row["kind"] != "C1":
                label = {"C0": "unprojected_corners", "C2": "mixed_corners", "C3": "unknown_corners"}[row["kind"]]
                result[label] += 1
                result[opacity + "_" + label] += 1
                continue
            result["tagged_corners"] += 1
            if opaque_only and int(row["opcode"]) & 2:
                result["excluded_semitransparent_corners"] += 1
                continue
            serial = int(row["tag"]) & ((1 << 62) - 1)
            offset = 16 + (serial - start) * RECORD.size
            if offset < 16 or offset + RECORD.size > len(data):
                result["missing_oracle_corners"] += 1
                continue
            raw = RECORD.unpack_from(data, offset)
            assert raw[0] == serial, "oracle observations must be contiguous"
            assert raw[2] == int(row["pc"]), "independent projection PCs disagree"
            xyz = (signed16(raw[3]), signed16(raw[3] >> 16), signed16(raw[4]))
            slots = tuple(zip(raw[13:21], raw[21:29]))
            matrix = raw[5:13]
            key = (int(row["vertex"]), int(row["context"]), int(row["pc"]), int(row["occurrence"]))
            mesh = meshes[key[1:3]]
            mesh["corners"] += 1
            mesh["opcodes"].add(int(row["opcode"]))
            mesh["frames"].add(int(row["frame"]))
            mesh["z"].add(xyz[2])
            p = int(row["pass"])
            sample = (xyz, slots, matrix)
            if key not in frames[p]:
                frames[p][key] = [sample, 1, int(row["frame"])]
            else:
                previous = frames[p][key]
                previous[1] += 1
                if previous[0] != sample:
                    previous[0] = None
        ordered = sorted(frames)
        changes = Counter()
        for previous, current in zip(ordered, ordered[1:]):
            if previous + 1 != current:
                continue
            a, b = frames[previous], frames[current]
            for key in a.keys() & b.keys():
                old, now = a[key][0], b[key][0]
                result["pairs"] += 1
                four = False
                if previous - 1 in frames and current + 1 in frames and key in frames[previous - 1] and key in frames[current + 1]:
                    result["four_frame_pairs"] += 1
                    four = True
                kind = None
                if old is None or now is None:
                    kind = "ambiguous_identity"
                elif any(not address for address, _ in old[1] + now[1]):
                    kind = "unknown_matrix_slot"
                elif old[1] != now[1]:
                    kind = "matrix_slot_mismatch"
                elif old[0] != now[0]:
                    kind = "input_xyz_mismatch"
                    changes[(key[1], key[2])] += b[key][1]
                if kind:
                    result[kind] += 1
                    result["outliers"] += 1
                    result["four_frame_outliers"] += int(four)
                    if sum(e["kind"] == kind for e in examples) < 16:
                        examples.append(dict(kind=kind, frame=b[key][2], key=key, previous=old, current=now))
                else:
                    result["correct_pairs"] += 1
        result["ambiguous_corners"] = sum(item[1] for f in frames.values() for item in f.values() if item[0] is None)
    output = dict(counts=dict(result), first=first, last=last, scope="opaque polygons" if opaque_only else "all polygons",
                  four_frame_outlier_percent=100 * result["four_frame_outliers"] / result["four_frame_pairs"] if result["four_frame_pairs"] else None,
                  outlier_percent=100 * result["outliers"] / result["pairs"] if result["pairs"] else None,
                  vertex_animation_candidates=[dict(context=c, pc=pc, changed_corner_uses=n,
                    mesh_corner_uses=meshes[c, pc]["corners"],
                    percent_of_scene_corners=100 * meshes[c, pc]["corners"] /
                        (result["all_corners"] - result["unprojected_corners"]),
                    opcodes=sorted(meshes[c, pc]["opcodes"]),
                    first_frame=min(meshes[c, pc]["frames"]), last_frame=max(meshes[c, pc]["frames"]),
                    input_z_range=[min(meshes[c, pc]["z"]), max(meshes[c, pc]["z"])])
                                               for (c, pc), n in changes.most_common()],
                  examples=examples)
    return output


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("trace", type=Path)
    parser.add_argument("oracle", type=Path)
    parser.add_argument("--first", type=int, default=2500)
    parser.add_argument("--last", type=int, default=2700)
    parser.add_argument("--opaque-only", action="store_true")
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    result = inspect(args.trace, args.oracle, args.first, args.last, args.opaque_only)
    args.output.write_text(json.dumps(result, indent=2) + "\n")
    print(json.dumps({k: v for k, v in result.items() if k not in ("examples", "vertex_animation_candidates")}, indent=2))
