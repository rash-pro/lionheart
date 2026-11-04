"""Compare a captured WAV against independently decoded FF8 SMN/SMR disc audio.

No disc payload is a fixture. Put the WAV under build/ or work/; the JSON receipt
is written next to it. Correlation allows the SPU's interpolation and volume gain.
"""
import argparse
import json
from pathlib import Path
import wave

import numpy as np

ROOT = Path(__file__).resolve().parents[1]
FILTERS = ((0, 0), (60, 0), (115, -52), (98, -55), (122, -60))


def reference_audio(disc, lba, first, count):
    history = [[0, 0], [0, 0]]
    channels = [[], []]
    with disc.open("rb") as stream:
        for frame in range(first + count):
            for channel in range(2):
                stream.seek((lba + frame * 10 + channel) * 2352 + 24)
                sector = stream.read(2048)
                assert sector[:4] == (b"SMN\x01" if channel == 0 else b"SMR\x01")
                assert int.from_bytes(sector[0x11c:0x11e], "little") == 0x1000
                size = int.from_bytes(sector[0x120:0x124], "little")
                assert size == 1680
                for offset in range(0x170, 0x170 + size, 16):
                    block = sector[offset:offset + 16]
                    shift = block[0] & 15
                    if shift > 12:
                        shift = 9
                    predictor = block[0] >> 4
                    assert predictor < len(FILTERS)
                    a, b = FILTERS[predictor]
                    for i in range(28):
                        nibble = (block[2 + i // 2] >> (i % 2 * 4)) & 15
                        if nibble >= 8:
                            nibble -= 16
                        sample = (nibble * 4096) // (1 << shift)
                        sample += (history[channel][0] * a + history[channel][1] * b + 32) // 64
                        sample = min(32767, max(-32768, sample))
                        history[channel] = [sample, history[channel][0]]
                        if frame >= first:
                            channels[channel].append(sample)
    return np.asarray(channels, dtype=np.float64).T


def match(samples, reference, sample_rate):
    # Quarter-rate correlation keeps the check small and tolerates sub-sample
    # interpolation delay. Normalize each candidate window independently.
    x = samples[::4].astype(np.float64)
    y = reference[::4].astype(np.float64)
    y -= y.mean()
    n = 1 << (len(x) + len(y) - 1).bit_length()
    dot = np.fft.irfft(np.fft.rfft(x, n) * np.conj(np.fft.rfft(y, n)), n)[:len(x) - len(y) + 1]
    total = np.r_[0.0, np.cumsum(x)]
    square = np.r_[0.0, np.cumsum(x * x)]
    energy = square[len(y):] - square[:-len(y)] - (total[len(y):] - total[:-len(y)]) ** 2 / len(y)
    denominator = np.sqrt(np.maximum(energy, 1e-30) * np.sum(y * y))
    correlation = dot / denominator
    best = int(np.argmax(np.abs(correlation)))
    return {"correlation": float(correlation[best]), "offset_seconds": best * 4 / sample_rate}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("disc_bin", type=Path)
    parser.add_argument("wav", type=Path)
    parser.add_argument("--lba", type=int, default=282220)
    parser.add_argument("--first-frame", type=int, default=300)
    parser.add_argument("--frames", type=int, default=100)
    parser.add_argument("--search-start", type=float, default=40)
    parser.add_argument("--search-end", type=float, default=65)
    args = parser.parse_args()
    assert args.wav.resolve().is_relative_to(ROOT / "build") or args.wav.resolve().is_relative_to(ROOT / "work")
    reference = reference_audio(args.disc_bin, args.lba, args.first_frame, args.frames)
    with wave.open(str(args.wav), "rb") as stream:
        assert (stream.getnchannels(), stream.getsampwidth(), stream.getframerate()) == (2, 2, 44100)
        stream.setpos(int(args.search_start * 44100))
        captured = np.frombuffer(stream.readframes(int((args.search_end - args.search_start) * 44100)), dtype="<i2").reshape(-1, 2)
    assert len(captured) >= len(reference) and len(reference) > 0
    results = [match(captured[:, channel], reference[:, channel], 44100) for channel in range(2)]
    for result in results:
        result["wav_seconds"] = args.search_start + result["offset_seconds"]
    report = {"lba": args.lba, "first_frame": args.first_frame, "frames": args.frames,
              "reference_samples": len(reference), "channels": results,
              "matched": all(abs(result["correlation"]) > 0.85 for result in results)}
    args.wav.with_suffix(".movie-match.json").write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps(report, indent=2))
    if not report["matched"]:
        raise SystemExit("captured audio did not match the selected disc movie audio")


if __name__ == "__main__":
    main()
