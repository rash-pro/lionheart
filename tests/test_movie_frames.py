"""The movie checker must reject misplaced tiles and accept decoder rounding."""
import struct
import zlib

import numpy as np
import pytest

from verify_movie_frames import compare_frames, pack_rgb15, tile_errors
from verify_movie_sync import matched_anchor


def write_png(path, pixels):
    def chunk(kind, payload):
        return (struct.pack(">I", len(payload)) + kind + payload +
                struct.pack(">I", zlib.crc32(kind + payload)))

    rows = np.zeros((224, 320 * 3 + 1), dtype=np.uint8)
    rows[:, 1:] = pixels.reshape(224, 320 * 3)
    header = struct.pack(">IIBBBBB", 320, 224, 8, 2, 0, 0, 0)
    path.write_bytes(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", header) +
                     chunk(b"IDAT", zlib.compress(rows.tobytes())) + chunk(b"IEND", b""))


def test_movie_band_is_not_hidden_by_whole_frame_average(tmp_path):
    random = np.random.default_rng(0x1820)
    reference = random.integers(8, 248, size=(3, 224, 320, 3), dtype=np.uint8)
    for number in range(12):
        write_png(tmp_path / f"frame-{number:06d}.png", reference[number // 4] + 1)
    assert compare_frames(reference, tmp_path, 0, 11)["bad_displayed_frames"] == 0
    displayed = reference[1].copy()
    displayed[:32, :-16] = reference[1, :32, 16:]
    write_png(tmp_path / "frame-000005.png", displayed)
    report = compare_frames(reference, tmp_path, 0, 11)
    assert report["checked_vblanks"] == 12
    assert report["bad_displayed_frames"] == 1
    assert report["failures"][0]["reference_frame"] == 1
    assert report["failures"][0]["bad_tiles_yx"] == [[y, x] for y in range(2) for x in range(19)]
    (tmp_path / "frame-000006.png").unlink()
    with pytest.raises(FileNotFoundError):
        compare_frames(reference, tmp_path, 0, 11)


def test_movie_checker_accepts_integer_color_rounding():
    reference = np.full((224, 320, 3), 128, dtype=np.uint8)
    assert (tile_errors(reference + 1, reference) == 1).all()
    with pytest.raises(ValueError, match="320x224"):
        tile_errors(reference[:16], reference[:16])


def test_movie_switch_to_rgb15_does_not_hide_a_misplaced_band(tmp_path):
    random = np.random.default_rng(0x15)
    reference = random.integers(8, 248, size=(1, 224, 320, 3), dtype=np.uint8)
    displayed = pack_rgb15(reference[0])
    write_png(tmp_path / "frame-000000.png", displayed)
    report = compare_frames(reference, tmp_path, 0, 0)
    assert report["bad_displayed_frames"] == 0
    assert report["rgb15_reference_vblanks"] == 1
    displayed[:32, :-16] = displayed[:32, 16:]
    write_png(tmp_path / "frame-000000.png", displayed)
    assert compare_frames(reference, tmp_path, 0, 0)["bad_displayed_frames"] == 1


@pytest.mark.parametrize("left,right,lag,expected", [
    (0.84, 0.93, 0.0, True),  # Other game audio can reduce one channel's correlation.
    (0.99, 0.99, 0.1, False),  # Both movie channels must agree on timing.
    (0.70, 0.80, 0.0, False),
    (float("nan"), 0.99, 0.0, False),
])
def test_audio_anchor_requires_a_strong_match_and_stereo_agreement(left, right, lag, expected):
    assert matched_anchor([{"correlation": left, "offset_seconds": 1.0},
                           {"correlation": right, "offset_seconds": 1.0 + lag}]) is expected
