"""Geometry identity, shared-edge rejection, exclusions and cut behavior."""
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[1]


def test_geometry_correspondence_and_fail_closed_components(tmp_path):
    binary = tmp_path / 'geometry'
    subprocess.run(['c++', '-std=c++17', '-O2', '-Wall', '-Wextra', '-Wpedantic', '-Werror',
                    '-fsanitize=undefined', '-fno-sanitize-recover=all', '-Iruntime', '-Ibackends/host',
                    'tests/frame_geometry_harness.cpp', '-o', str(binary)], cwd=ROOT, check=True)
    subprocess.run([str(binary)], check=True)
