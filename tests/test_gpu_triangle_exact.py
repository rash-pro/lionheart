"""Compare full synthetic VRAM, including feedback, against scalar interpolation."""
import subprocess


def test_triangle_interpolation_is_exact(tmp_path):
    binary = tmp_path / 'triangles'
    subprocess.run(['cc', '-std=c99', '-O2', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=undefined', '-fno-sanitize-recover=all', '-Iruntime',
                    'tests/gpu_triangle_harness.c', '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True, timeout=60)
