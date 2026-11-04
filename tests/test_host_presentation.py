"""Fullscreen and final texture filtering through SDL's dummy video driver."""
import os
from pathlib import Path
import shlex
import subprocess

ROOT = Path(__file__).resolve().parents[1]


def test_fullscreen_and_filter(tmp_path):
    binary = tmp_path / "presentation"
    sdl = shlex.split(subprocess.check_output(["pkg-config", "--cflags", "--libs", "sdl2"], text=True))
    flags = ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"] if os.environ.get("FF8_TEST_SANITIZE") else []
    subprocess.run(["cc", "-std=c99", "-O1", "-g", "-Wall", "-Wextra", "-Werror", *flags,
                    "-Iruntime", "-Ibackends/host", "tests/host_presentation_harness.c",
                    "backends/host/display.c", "backends/host/screenshot.c", "backends/host/osd.c", "backends/host/viewport.c", "runtime/gpu.c", "runtime/gpu_raster_soft.c",
                    "-Wl,--wrap=SDL_SetTextureScaleMode,--wrap=SDL_CreateWindow,--wrap=SDL_SetWindowFullscreen,--wrap=SDL_RenderCopy", "-o", str(binary), *sdl, "-lz"], cwd=ROOT, check=True)
    subprocess.run([str(binary)], check=True, timeout=30,
                   env={**os.environ, "SDL_VIDEODRIVER": "dummy", "SDL_AUDIODRIVER": "dummy"})
