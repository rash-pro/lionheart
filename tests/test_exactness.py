"""Owned-game exactness: compare the native host against its interpreter fallback."""
from pathlib import Path
import subprocess
from conftest import ROOT, owned_disc, owned_path
from tools.compare import compare, run


def test_native_matches_interpreter(tmp_path):
    exe, disc = owned_path('SLUS_008.92'), owned_disc(1)
    native = ROOT / 'build/dev/ff8'
    assert native.is_file(), 'run python tools/setup.py --disc1 DISC first'
    # Keep the executable wrapper synthetic. Both runs use the same binary/configuration.
    wrapper = tmp_path / 'interpret'
    import shlex
    wrapper.write_text('#!/bin/sh\nexec ' + shlex.quote(str(native)) + ' "$@" --interpreter\n')
    wrapper.chmod(0o755)
    script = ROOT / 'tests/scripts/new-game.txt'
    before = run(wrapper, exe, [disc], script, 15000, tmp_path / 'interpreter')
    after = run(native, exe, [disc], script, 15000, tmp_path / 'native')
    assert compare(before, after)['passed']
