"""SPU reference vectors and DMA/MMIO integration without disc-derived fixtures."""
import os
from pathlib import Path
import subprocess
import shlex
import wave
import struct

import pytest

ROOT = Path(__file__).resolve().parents[1]


@pytest.fixture(scope="module")
def spu_binary(tmp_path_factory):
    output = tmp_path_factory.mktemp("spu") / "harness"
    flags = ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"] if os.environ.get("FF8_TEST_SANITIZE") else []
    sources = sorted(p for p in (ROOT / "runtime").glob("*.c") if p.name != "spu.c")
    subprocess.run(["cc", "-std=c99", "-O1", "-g", "-Wall", "-Wextra", "-Werror", *flags,
                    "-I", str(ROOT / "runtime"), str(ROOT / "tests/spu_harness.c"),
                    *map(str, sources), "-o", str(output)], check=True)
    return output


@pytest.mark.parametrize("case", ["deadlines", "adpcm", "gaussian", "adsr", "sample", "loops", "transfers", "dma", "stream_dma", "cd", "modulation", "reverb", "reverb_addresses", "volume_registers"])
def test_spu(spu_binary, case):
    subprocess.run([str(spu_binary), case], check=True, timeout=30)


@pytest.fixture(scope="module")
def audio_binary(tmp_path_factory):
    output = tmp_path_factory.mktemp("audio") / "harness"
    sdl = shlex.split(subprocess.check_output(["pkg-config", "--cflags", "--libs", "sdl2"], text=True))
    flags = ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"] if os.environ.get("FF8_TEST_SANITIZE") else []
    subprocess.run(["cc", "-std=c99", "-O1", "-g", "-Wall", "-Wextra", "-Werror", *flags,
                    "-I", str(ROOT / "runtime"), str(ROOT / "tests/audio_harness.c"),
                    str(ROOT / "runtime/spu.c"), *sdl, "-o", str(output)], check=True)
    return output


@pytest.mark.parametrize("mode", ["headless", "mute", "device"])
def test_audio_wav_is_lossless(audio_binary, tmp_path, mode):
    path = tmp_path / f"{mode}.wav"
    subprocess.run([str(audio_binary), mode, str(path)], check=True, timeout=30,
                   env={**os.environ, "SDL_AUDIODRIVER": "dummy"})
    with wave.open(str(path), "rb") as audio:
        assert (audio.getnchannels(), audio.getsampwidth(), audio.getframerate(), audio.getnframes()) == (2, 2, 44100, 44100)
        assert audio.readframes(44100) == struct.pack("<hh", 4999, -5000) * 44100


@pytest.mark.parametrize("mode", [pytest.param("paced", marks=pytest.mark.realtime),
                                  pytest.param("pal", marks=pytest.mark.realtime),
                                  "fast", "resample", "overrun"])
def test_audio_clock_and_fast_mode(audio_binary, tmp_path, mode):
    subprocess.run([str(audio_binary), mode, str(tmp_path / f"{mode}.wav")], check=True, timeout=15,
                   env={**os.environ, "SDL_AUDIODRIVER": "dummy"}, capture_output=True)
