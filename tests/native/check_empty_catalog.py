#!/usr/bin/env python3
"""Compare complete empty catalog outputs; delete this bridge with pytest."""
import argparse
import os
from pathlib import Path
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[2]
sys.path[:0] = [str(ROOT), str(ROOT / 'tests')]
from recomp.discover import Image
from recomp.emit import emit
from test_host_player import fake_exe

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('runner', type=Path)
args = parser.parse_args()
with tempfile.TemporaryDirectory() as folder:
    folder = Path(folder)
    env = dict(os.environ, LH_EMPTY_CATALOG=str(folder / 'native'))
    subprocess.run([str(args.runner.resolve()), '--run',
                    'test_startup::test_dev_keeps_unidentified_arbitrary_extension_and_order'],
                   env=env, check=True)
    exe = folder / 'guest.exe'
    exe.write_bytes(fake_exe())
    python = folder / 'python'
    python.mkdir()
    emit(Image.read(exe), {}, python)
    native = folder / 'native'
    names = sorted(p.name for p in python.iterdir())
    assert names == sorted(p.name for p in native.iterdir())
    for name in names:
        assert (python / name).read_bytes() == (native / name).read_bytes(), name
    print(f'PASS: {len(names)} empty catalog files are byte-identical (shared host/startup input)')
