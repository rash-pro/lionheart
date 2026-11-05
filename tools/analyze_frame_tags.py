"""Pixel deviation diagnostics; acceptance is decided by the independent GTE oracle."""
import argparse
from collections import Counter, defaultdict
import csv
import json
import math
from pathlib import Path


def quantiles(values):
    values = sorted(values)
    return {str(q): values[round((len(values) - 1) * q)] if values else None
            for q in (0, 0.5, 0.9, 0.95, 0.99, 0.995, 1)}


def analyze(path, first, last):
    counts = Counter()
    passes = defaultdict(dict)
    ranges = {}
    ambiguous = 0
    for row in csv.DictReader(path.open()):
        frame = int(row["frame"])
        if not row["kind"].startswith("C") or not first <= frame < last:
            continue
        counts[row["kind"]] += 1
        if row["kind"] != "C1":
            continue
        p, serial = int(row["pass"]), int(row["tag"])
        base = (int(row["vertex"]), int(row["context"]), int(row["pc"]), int(row.get("occurrence", 0)))
        point = (int(row["x"]) - int(row["source_xy"]), int(row["y"]) - int(row["source_z"]))
        lo, hi = ranges.get(p, (frame, frame))
        ranges[p] = min(lo, frame), max(hi, frame)
        if serial not in passes[p]:
            passes[p][serial] = [base, point]
        elif passes[p][serial][1] != point:
            passes[p][serial][1] = None
    frames = []
    for p, vertices in sorted(passes.items()):
        occurrence = Counter()
        resolved = {}
        for serial, (base, point) in sorted(vertices.items()):
            index = occurrence[base]
            occurrence[base] += 1
            if point is None:
                ambiguous += 1
            else:
                key = base if len(base) == 4 else (*base, index)
                if key in resolved and resolved[key] != point:
                    resolved[key] = None
                else:
                    resolved[key] = point
        frames.append((p, ranges[p], {k:v for k,v in resolved.items() if v is not None}))
    errors, examples, windows = [], [], []
    offscreen = 0
    per_identity = defaultdict(list)
    # N-2, N-1, N, N+1 must all contain the identity. Check N against
    # independently consumed packet positions at N-1 and N+1, not GTE output.
    # Do not discard high residuals by calling them cuts. Cut exclusions must
    # be supplied from separate visual/camera evidence.
    for a, b, c, d in zip(frames, frames[1:], frames[2:], frames[3:]):
        if not a[0] + 1 == b[0] == c[0] - 1 == d[0] - 2:
            continue
        common = a[2].keys() & b[2].keys() & c[2].keys() & d[2].keys()
        duration = d[1][1] - b[1][1]
        if duration <= 0:
            continue
        alpha = (c[1][1] - b[1][1]) / duration
        outliers = 0
        for key in common:
            if any(not (0 <= point[0] < 320 and 0 <= point[1] < 240)
                   for point in (a[2][key], b[2][key], c[2][key], d[2][key])):
                offscreen += 1
                continue
            before, now, after = b[2][key], c[2][key], d[2][key]
            expected = [before[i] + alpha * (after[i] - before[i]) for i in (0, 1)]
            error = math.hypot(now[0] - expected[0], now[1] - expected[1])
            errors.append(error)
            per_identity[key].append((c[1][1], error))
            if error > 3:
                outliers += 1
                if len(examples) < 64:
                    examples.append(dict(frame=c[1][1], key=key, previous=before, current=now, next=after, residual=error))
        windows.append(dict(frame=c[1][1], pass_id=c[0], common=len(common), outliers=outliers))
    scene = counts["C1"] + counts["C2"] + counts["C3"]
    result = dict(first=first, last=last, corners=dict(counts),
                  unknown_mixed_percent=100 * (counts["C2"] + counts["C3"]) / scene if scene else None,
                  samples=len(errors), offscreen_samples=offscreen, ambiguous_projection_uses=ambiguous,
                  residual_native_pixels=quantiles(errors), diagnostic_threshold_native_pixels=3,
                  residuals_above_3px=sum(e > 3 for e in errors),
                  residuals_above_3px_percent=100 * sum(e > 3 for e in errors) / len(errors) if errors else None,
                  distribution={str(b): sum(e <= b for e in errors) for b in (0, .25, .5, 1, 2, 3, 4, 8, 16, 32)},
                  cut_exclusions=[], windows=windows, examples=examples)
    return result


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("trace", type=Path)
    parser.add_argument("--first", type=int, default=2400)
    parser.add_argument("--last", type=int, default=2700)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    result = analyze(args.trace, args.first, args.last)
    args.output.write_text(json.dumps(result, indent=2) + "\n")
    print(json.dumps({k: v for k, v in result.items() if k not in ("examples", "windows", "robust_outlier_examples")}, indent=2))
