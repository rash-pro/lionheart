"""Synthetic MDEC/CD vectors. No disc data or emulator source is an oracle."""
import os
from pathlib import Path
import random
import struct
import subprocess

import pytest

ROOT = Path(__file__).resolve().parents[1]


@pytest.fixture(scope="module")
def media_binary(tmp_path_factory):
    output = tmp_path_factory.mktemp("media") / "harness"
    flags = ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"] if os.environ.get("FF8_TEST_SANITIZE") else []
    subprocess.run(["cc", "-std=c99", "-O1", "-g", "-Wall", "-Wextra", "-Wpedantic", "-Werror", *flags,
                    "-I", str(ROOT / "runtime"), str(ROOT / "tests/media_harness.c"),
                    *map(str, sorted((ROOT / "runtime").glob("*.c"))),
                    "-Wl,--wrap=spu_cd_input", "-o", str(output)], check=True)
    return output


def run(binary, *args, data=None):
    return subprocess.run([str(binary), *map(str, args)], input=data, stdout=subprocess.PIPE,
                          check=True, timeout=30).stdout


@pytest.mark.parametrize("case", ["mdec", "dma", "movie-overlap", "movie-priority", "movie-cancel", "movie-late-gpu"])
def test_mdec_registers_and_dma(media_binary, case):
    run(media_binary, case)


def clip(n, low=-32768, high=32767):
    return max(low, min(high, n))


def test_idct_fixed_point(media_binary):
    # Non-symmetric programmable matrices catch accidental transposition and
    # preserve the rounding between passes; large inputs exercise wrap/clamp.
    randomizer = random.Random(0x1820)
    for _ in range(24):
        coeff = [randomizer.randrange(-16384, 16384) for _ in range(64)]
        matrix = [randomizer.randrange(-32768, 32768) for _ in range(64)]
        expected = coeff
        for _pass in range(2):
            expected = [clip((sum(expected[r + k * 8] * (matrix[x + k * 8] // 8) for k in range(8)) + 16384) // 32768)
                        for r in range(8) for x in range(8)]
        expected = [clip((v + 256) % 512 - 256, -128, 127) for v in expected]
        result = run(media_binary, "idct", data=struct.pack("<128h", *coeff, *matrix))
        assert list(struct.unpack("<64h", result)) == expected


def xa_sector(bits, stereo, half_rate=False):
    raw = bytearray(2352)
    raw[15:24] = bytes([2, 1, 2, 0x64, (bits == 8) * 16 + stereo + half_rate * 4, 1, 2, 0x64, 0])
    for group in range(18):
        start = 24 + group * 128
        for unit in range(32 // bits):
            # Covers all predictor filters, all valid shifts, and reserved 13..15.
            raw[start + 4 + unit] = ((group + unit) % 4) * 16 + (group + unit) % 16
        for sample in range(28):
            for lane in range(4):
                raw[start + 16 + sample * 4 + lane] = (sample * 37 + group * 19 + lane * 67) & 255
    return raw


def xa_reference(raw, history):
    stereo = raw[19] & 1
    bits = 8 if raw[19] & 16 else 4
    channels = 2 if stereo else 1
    decoded = [[], []]
    for group in range(18):
        p = raw[24 + group * 128:24 + (group + 1) * 128]
        for unit in range(32 // bits):
            channel = unit % channels
            shift, predictor = p[4 + unit] & 15, (p[4 + unit] >> 4) & 3
            shift = 9 if shift > 12 else shift
            for i in range(28):
                packed = int.from_bytes(p[16 + i * 4:20 + i * 4], "little")
                sample = (packed >> (unit * bits)) & ((1 << bits) - 1)
                if sample & (1 << (bits - 1)):
                    sample -= 1 << bits
                old, older = history[channel]
                prediction = (old * (0, 60, 115, 98)[predictor] + older * (0, 0, -52, -55)[predictor] + 32) // 64
                value = clip((sample * (1 << (16 - bits))) // (1 << shift) + prediction)
                history[channel] = [value, old]
                decoded[channel].append(value)
    if not stereo:
        decoded[1] = decoded[0]
    return list(sum(zip(*decoded), ()))


@pytest.mark.parametrize("bits,stereo", [(4, False), (4, True), (8, False), (8, True)])
def test_xa_predictor_arithmetic_and_history(media_binary, bits, stereo):
    raw = xa_sector(bits, stereo)
    history = [[0, 0], [0, 0]]
    expected = xa_reference(raw, history) + xa_reference(raw, history)
    actual = run(media_binary, "xa", data=raw)
    assert list(struct.unpack(f"<{len(expected)}h", actual)) == expected


@pytest.mark.parametrize("bits,stereo,half", [(4, True, False), (4, False, True), (8, True, True), (8, False, False)])
def test_xa_interleave_resampling_and_volume(media_binary, tmp_path, bits, stereo, half):
    data = bytearray(2352)
    data[15] = 2
    data[18] = 8
    data[24:28] = bytes.fromhex("44332211")
    audio = xa_sector(bits, stereo, half)
    rejected = bytearray(audio)
    rejected[17] = 3
    path = tmp_path / "synthetic.bin"
    path.write_bytes(data + audio + rejected + data)
    plain = run(media_binary, "stream", path, "0xc8", 0)
    frames = (18 * (32 // bits) * 28 // (2 if stereo else 1)) * (2 if half else 1) * 7 // 6
    assert len(plain) == frames * 4
    assert any(plain)
    assert run(media_binary, "stream", path, "0xc0", 0) == plain  # Automatic channel selection without Setfilter mode.
    assert run(media_binary, "stream", path, "0x88", 0) == b""  # Filtering with XA disabled discards RT audio.
    assert run(media_binary, "stream", path, "0x80", 0) == b""  # Without either mode bit, all sectors reach the CPU.
    unchanged = run(media_binary, "stream", path, "0xc8", 2)
    assert unchanged == plain  # Volume writes are latched only by CHNGATV.
    swapped = run(media_binary, "stream", path, "0xc8", 1)
    expected = b"".join(plain[i + 2:i + 4] + plain[i:i + 2] for i in range(0, len(plain), 4))
    assert swapped == expected
    assert not any(run(media_binary, "stream", path, "0xc8", 3))


def test_xa_resampling_impulse(media_binary, tmp_path):
    data = bytearray(2352)
    data[15], data[18] = 2, 8
    data[24:28] = bytes.fromhex("44332211")
    audio = bytearray(2352)
    audio[15:20] = bytes([2, 1, 2, 0x64, 1])
    audio[24 + 16] = 7  # Left impulse 28672, right silent, all predictors zero.
    rejected = bytearray(audio)
    rejected[17] = 3
    path = tmp_path / "impulse.bin"
    path.write_bytes(data + audio + rejected + data)
    pcm = run(media_binary, "stream", path, "0xc8", 0)
    first = struct.unpack("<14h", pcm[:28])
    # First six inputs place the impulse at tap six. Seven filter coefficients
    # times 28672, floored after division by 32768, yield this exact vector.
    assert first[::2] == (-2, 2, -5, 4, 8, 22, -60)
    assert first[1::2] == (0,) * 7
