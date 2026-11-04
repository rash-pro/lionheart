"""Measure audio/video offset drift using independently matched movie frames.

PNG N is presented at hardware VBlank N+1, line 240 of the first NTSC field.
This checks guest playback timing and captured PCM, not host audio-device pacing.
"""
import argparse
import json
import math
from pathlib import Path
import wave

import numpy as np

from verify_movie_audio import match, reference_audio

ROOT = Path(__file__).resolve().parents[1]


def matched_anchor(channels):
    # Game audio can accompany the movie. Require an unambiguous match in
    # at least one channel and independent stereo peaks within one FFT step.
    return (all(math.isfinite(channel["correlation"]) for channel in channels) and
            max(abs(channel["correlation"]) for channel in channels) > 0.85 and
            abs(channels[0]["offset_seconds"] - channels[1]["offset_seconds"]) <= 4 / 44100)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("disc_bin", type=Path)
    parser.add_argument("wav", type=Path)
    parser.add_argument("video_report", type=Path)
    parser.add_argument("--anchors", nargs="+", type=int, default=[300, 2500])
    parser.add_argument("--window-frames", type=int, default=30)
    parser.add_argument("--max-drift", type=float, default=1 / 15,
                        help="maximum offset change in seconds (default: one 15 fps movie frame)")
    parser.add_argument("--report", type=Path, required=True)
    args = parser.parse_args()
    if not args.report.resolve().is_relative_to(ROOT / "build"):
        parser.error("write evidence under build/")
    video = json.loads(args.video_report.read_text())
    if video["bad_displayed_frames"]:
        parser.error("video comparison must pass before measuring sync")
    if len(args.anchors) < 2 or sorted(set(args.anchors)) != args.anchors:
        parser.error("provide at least two increasing, distinct anchor frames")
    anchors = []
    with wave.open(str(args.wav), "rb") as stream:
        if (stream.getnchannels(), stream.getsampwidth(), stream.getframerate()) != (2, 2, 44100):
            parser.error("expected 44100 Hz stereo PCM16")
        for frame in args.anchors:
            png = video["first_png_by_reference_frame"][str(frame)]
            seconds = (png + 240 / 263) * 1001 / 60000
            start = max(0, seconds - 4)
            stream.setpos(int(start * 44100))
            captured = np.frombuffer(stream.readframes(int((8 + args.window_frames / 15) * 44100)),
                                     dtype="<i2").reshape(-1, 2)
            reference = reference_audio(args.disc_bin, video["lba"], frame, args.window_frames)
            if len(captured) < len(reference):
                parser.error("WAV does not cover the selected audio anchor")
            channels = [match(captured[:, channel], reference[:, channel], 44100) for channel in range(2)]
            for channel in channels:
                channel["wav_seconds"] = start + channel["offset_seconds"]
                channel["audio_minus_video_seconds"] = channel["wav_seconds"] - seconds
            anchors.append({"reference_frame": frame, "first_png": png,
                            "video_seconds": seconds, "channels": channels})
    drift = [anchors[-1]["channels"][ch]["audio_minus_video_seconds"] -
             anchors[0]["channels"][ch]["audio_minus_video_seconds"] for ch in range(2)]
    ranges = [max(anchor["channels"][ch]["audio_minus_video_seconds"] for anchor in anchors) -
              min(anchor["channels"][ch]["audio_minus_video_seconds"] for anchor in anchors) for ch in range(2)]
    video_elapsed = anchors[-1]["video_seconds"] - anchors[0]["video_seconds"]
    expected_elapsed = (args.anchors[-1] - args.anchors[0]) / 15
    report = {"anchors": anchors, "drift_seconds_by_channel": drift,
              "offset_range_seconds_by_channel": ranges,
              "max_drift_seconds": args.max_drift, "reference_elapsed_seconds": expected_elapsed,
              "video_elapsed_seconds": video_elapsed,
              "observed_guest_movie_fps": (args.anchors[-1] - args.anchors[0]) / video_elapsed,
              "passed": all(value <= args.max_drift for value in ranges) and
                        all(matched_anchor(anchor["channels"]) for anchor in anchors)}
    args.report.write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps(report, indent=2))
    if not report["passed"]:
        raise SystemExit("movie audio/video drift or audio correlation failed")


if __name__ == "__main__":
    main()
