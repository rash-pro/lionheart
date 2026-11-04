"""GPUSTAT line parity must release a VSync wait across a display-mode change."""
from pathlib import Path
import subprocess

import pytest

ROOT = Path(__file__).resolve().parents[1]


@pytest.fixture(scope='module')
def status_binary(tmp_path_factory):
    output = tmp_path_factory.mktemp('gpu-status') / 'harness'
    subprocess.run(['cc', '-std=c99', '-O1', '-DFF8_STATS', '-I', str(ROOT / 'runtime'),
                    str(ROOT / 'tests/gpu_status_scanline.c'),
                    *map(str, sorted((ROOT / 'runtime').glob('*.c'))), '-o', str(output)], check=True)
    return output


@pytest.mark.parametrize('mode', ['ntsc', 'pal', '480i', 'transition'])
def test_status_parity(status_binary, mode):
    subprocess.run([str(status_binary), mode], check=True, timeout=15)
