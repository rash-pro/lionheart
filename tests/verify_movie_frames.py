"""Compare every host movie PNG with independently decoded SMJ disc frames.

Requires FFmpeg's MDEC decoder. Disc-derived AVI/RGB files and the JSON receipt
must stay under build/. Host PNGs use RGB8, filter None, and no interlace.
The comparison tolerates integer YUV/IDCT rounding, but checks each 16x16 tile
separately so a narrow misplaced band cannot disappear in a whole-frame average.
For displays using only the 32 RGB15 channel levels, it also compares the
reference after MDEC five-bit rounding and display expansion. FF8 switches
back to RGB15 during the last four frames of the gate-closing movie.
"""
import argparse
import json
from pathlib import Path
import struct
import subprocess
import zlib

import numpy as np

ROOT = Path(__file__).resolve().parents[1]
WIDTH, HEIGHT = 320, 224
RGB15 = np.zeros(256, dtype=bool)
RGB15[[(value << 3) | (value >> 2) for value in range(32)]] = True


def pack_rgb15(reference):
    packed = np.minimum((reference.astype(np.uint16) + 4) // 8, 31)
    return ((packed << 3) | (packed >> 2)).astype(np.uint8)


def read_host_png(path):
    data = path.read_bytes()
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        raise ValueError(f"invalid PNG: {path}")
    pos, compressed = 8, []
    while pos < len(data):
        size = int.from_bytes(data[pos:pos + 4], "big")
        kind, payload = data[pos + 4:pos + 8], data[pos + 8:pos + 8 + size]
        pos += size + 12
        if kind == b"IHDR":
            w, h, depth, color, compression, filtering, interlace = struct.unpack(">IIBBBBB", payload)
            if (depth, color, compression, filtering, interlace) != (8, 2, 0, 0, 0):
                raise ValueError(f"expected a host RGB8 PNG: {path}")
        elif kind == b"IDAT":
            compressed.append(payload)
    rows = np.frombuffer(zlib.decompress(b"".join(compressed)), dtype=np.uint8).reshape(h, w * 3 + 1)
    if rows[:, 0].any():
        raise ValueError(f"expected host PNG filter None: {path}")
    return rows[:, 1:].reshape(h, w, 3)


def chunk(tag, data):
    return tag + struct.pack("<I", len(data)) + data + (b"\0" if len(data) & 1 else b"")


def movie_avi(disc, lba, count, destination):
    def listing(tag, data):
        return chunk(b"LIST", tag + data)

    avih = chunk(b"avih", struct.pack("<14I", 66667, 0, 0, 0, count, 0, 1, 0, WIDTH, HEIGHT, 0, 0, 0, 0))
    strh = chunk(b"strh", b"vidsMDEC" + struct.pack("<IHHIIIIIIIIhhhh",
                  0, 0, 0, 0, 1, 15, 0, count, 0, 0xffffffff, 0, 0, 0, WIDTH, HEIGHT))
    strf = chunk(b"strf", struct.pack("<IiiHH4sIiiII",
                  40, WIDTH, HEIGHT, 1, 24, b"MDEC", WIDTH * HEIGHT * 3, 0, 0, 0, 0))
    packets = []
    with disc.open("rb") as stream:
        for frame in range(count):
            pieces = []
            for sector_index in range(2, 10):
                stream.seek((lba + frame * 10 + sector_index) * 2352 + 24)
                sector = stream.read(2048)
                if (len(sector) != 2048 or sector[:6] != b"SMJ\1" + bytes([sector_index, 9])
                        or int.from_bytes(sector[6:8], "little") != frame):
                    raise ValueError(f"unexpected SMJ header at movie frame {frame}, sector {sector_index}")
                pieces.append(sector[8:])
            packets.append(chunk(b"00dc", b"".join(pieces)))
    headers = listing(b"hdrl", avih + listing(b"strl", strh + strf))
    destination.write_bytes(chunk(b"RIFF", b"AVI " + headers + listing(b"movi", b"".join(packets))))


def tile_errors(displayed, reference):
    if displayed.shape != (HEIGHT, WIDTH, 3) or reference.shape != displayed.shape:
        raise ValueError("movie dimensions must be 320x224 RGB")
    difference = np.abs(displayed.astype(np.int16) - reference.astype(np.int16))
    return difference.reshape(HEIGHT // 16, 16, WIDTH // 16, 16, 3).mean(axis=(1, 3, 4))


def compare_frames(reference, directory, first_png, last_png, tolerance=3.0, radius=4):
    failures, anchors = [], {}
    maximum, total_error, checked, packed_vblanks = 0.0, 0.0, 0, 0
    previous, cached = None, None
    for number in range(first_png, last_png + 1):
        path = directory / f"frame-{number:06d}.png"
        displayed = read_host_png(path)
        expected = (number - first_png) // 4
        low, high = max(0, expected - radius), min(len(reference), expected + radius + 1)
        if low >= high:
            raise ValueError("capture extends past the reference movie")
        if previous is not None and np.array_equal(displayed, previous) and low <= cached[0] < high:
            best, errors, packed = cached
        else:
            candidates = reference[low:high, ::8, ::8].astype(np.int16)
            scores = np.abs(candidates - displayed[None, ::8, ::8].astype(np.int16)).mean(axis=(1, 2, 3))
            can_be_rgb15 = bool(RGB15[displayed].all())
            if can_be_rgb15:
                packed_candidates = pack_rgb15(candidates).astype(np.int16)
                packed_scores = np.abs(packed_candidates - displayed[None, ::8, ::8]).mean(axis=(1, 2, 3))
                scores = np.minimum(scores, packed_scores)
            best = low + int(scores.argmin())
            errors = tile_errors(displayed, reference[best])
            packed = False
            if can_be_rgb15:
                packed_errors = tile_errors(displayed, pack_rgb15(reference[best]))
                if packed_errors.mean() < errors.mean():
                    errors, packed = packed_errors, True
            previous, cached = displayed, (best, errors, packed)
        maximum = max(maximum, float(errors.max()))
        total_error += float(errors.mean())
        checked += 1
        packed_vblanks += packed
        anchors.setdefault(str(best), number)
        bad = np.argwhere(errors > tolerance)
        if len(bad):
            failures.append({"png": str(path), "reference_frame": best,
                             "max_tile_error": float(errors.max()), "bad_tiles_yx": bad.tolist()})
    return {"checked_vblanks": checked, "first_png": first_png, "last_png": last_png,
            "reference_frames": len(reference), "tolerance_rgb_levels": tolerance,
            "max_tile_error": maximum, "mean_pixel_error": total_error / checked,
            "rgb15_reference_vblanks": packed_vblanks,
            "bad_displayed_frames": len(failures), "failures": failures,
            "first_png_by_reference_frame": anchors}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("disc_bin", type=Path)
    parser.add_argument("frames", type=Path)
    parser.add_argument("--lba", type=int, default=282220)
    parser.add_argument("--count", type=int, default=2897)
    parser.add_argument("--first-png", type=int, required=True, help="PNG index showing movie frame zero")
    parser.add_argument("--last-png", type=int, required=True)
    parser.add_argument("--tolerance", type=float, default=3.0)
    parser.add_argument("--report", type=Path, required=True)
    parser.add_argument("--reuse-reference", type=Path, help="existing RGB24 reference from this movie")
    args = parser.parse_args()
    if not args.report.resolve().is_relative_to(ROOT / "build"):
        parser.error("write disc-derived evidence under build/")
    if not (args.count > 0 and 0 <= args.first_png <= args.last_png):
        parser.error("invalid frame range")
    args.report.parent.mkdir(parents=True, exist_ok=True)
    rgb = args.reuse_reference or args.report.with_suffix(".rgb")
    if not args.reuse_reference:
        avi = args.report.with_suffix(".avi")
        movie_avi(args.disc_bin, args.lba, args.count, avi)
        subprocess.run(["ffmpeg", "-nostdin", "-y", "-hide_banner", "-loglevel", "warning",
                        "-c:v", "mdec", "-i", str(avi), "-pix_fmt", "rgb24", "-fps_mode", "passthrough",
                        "-f", "rawvideo", str(rgb)], check=True)
    if rgb.stat().st_size != args.count * HEIGHT * WIDTH * 3:
        parser.error("reference RGB size does not match movie frame count")
    reference = np.memmap(rgb, dtype=np.uint8, mode="r").reshape(args.count, HEIGHT, WIDTH, 3)
    report = compare_frames(reference, args.frames, args.first_png, args.last_png, args.tolerance)
    report.update(lba=args.lba, reference_rgb=str(rgb))
    if not args.reuse_reference:
        report["reference_avi"] = str(args.report.with_suffix(".avi"))
    args.report.write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps({key: value for key, value in report.items()
                      if key not in ("failures", "first_png_by_reference_frame")}, indent=2))
    if report["bad_displayed_frames"]:
        raise SystemExit("display contains misaligned movie tiles; see the JSON receipt")


if __name__ == "__main__":
    main()
