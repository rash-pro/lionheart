"""Synthetic overlapping registries retain the linear dispatch semantics."""
import subprocess
import pytest


@pytest.fixture(scope='module', params=[0, 1], ids=['entry', 'precise'])
def dispatch_probe(tmp_path_factory, request):
    binary = tmp_path_factory.mktemp('dispatch-index') / 'dispatch'
    subprocess.run(['cc', '-std=c99', '-O2', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=undefined', '-fno-sanitize-recover=all', '-Iruntime',
                    f'-DTEST_PRECISE={request.param}',
                    'runtime/cpu.c', 'runtime/interp.c', 'tests/dispatch_index_harness.c',
                    '-o', str(binary)], check=True)
    return binary


@pytest.mark.parametrize('seed', [1, 0x9E3779B9, 0xDEADBEEF])
def test_dispatch_index_matches_linear_catalog(dispatch_probe, seed):
    subprocess.run([str(dispatch_probe), '10000', str(seed)], check=True, timeout=60)
