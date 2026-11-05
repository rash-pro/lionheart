"""Catalog rejection preserves disjoint spans, invalidation, and registry changes."""
import subprocess


def test_precise_catalog_bounds(tmp_path):
    binary = tmp_path / 'bounds'
    subprocess.run(['cc', '-std=c99', '-O2', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=undefined', '-fno-sanitize-recover=all', '-Iruntime',
                    'runtime/cpu.c', 'runtime/interp.c', 'tests/precise_bounds_harness.c',
                    '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
