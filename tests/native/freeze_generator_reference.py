#!/usr/bin/env python3
"""Record Python emission digests and compare native spec descriptions before Python removal."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys

import pytest

ROOT = Path(__file__).resolve().parents[2]
sys.path[:0] = [str(ROOT), str(ROOT / 'tests')]


def canonical(spec, temporary):
    return json.dumps(spec, sort_keys=True, separators=(',', ':'), ensure_ascii=True).replace(str(temporary), '$TMP')


class Capture:
    def __init__(self, output):
        self.output = output
        self.rows = []
        self.manifest = []
        self.specs = {}

    def save(self, item, label, spec, temporary, files):
        key = item.nodeid.replace('tests/', '', 1).replace('.py::', '::', 1) + '/' + label
        self.specs[key] = canonical(spec, temporary)
        for name, data in sorted(files.items()):
            self.manifest.append(f'{key}\t{name}\t{hashlib.sha256(data).hexdigest()}\n')

    def pytest_collection_modifyitems(self, items):
        import test_generator_parity as module
        self.module = module
        self.original = module.assert_parity
        def parity(tools, spec, temporary, name):
            self.original(tools, spec, temporary, name)
            self.save(self.current, name, spec, temporary, module.tree(temporary / f'{name}-py'))
        module.assert_parity = parity
        self.real_run = subprocess.run
        def run(args, *a, **kw):
            result = self.real_run(args, *a, **kw)
            if (self.current and isinstance(args, list) and len(args) == 3
                    and str(args[0]).endswith('lionheart-gen-spec')):
                spec = json.loads(Path(args[1]).read_text())
                if Path(args[1]).name == 'invalid.json':
                    self.save(self.current, 'invalid', spec, Path(args[1]).parent, {})
                elif Path(args[1]).name == 'spec.json':
                    self.save(self.current, 'rerun', spec, Path(args[1]).parent, {})
                elif 'lower_source' in spec and result.returncode == 0:
                    expected, _ = module.lower(spec['lower_source'], spec.get('max_registers', 8))
                    self.save(self.current, 'lower', spec, Path(args[1]).parent, {'lowered.c': expected.encode()})
                elif Path(args[1]).name == 'behavior.json':
                    self.save(self.current, 'behavior', spec, Path(args[1]).parent, {})
            return result
        subprocess.run = run
        self.current = None
        for item in items:
            params = getattr(getattr(item, 'callspec', None), 'params', {})
            params = {k: v for k, v in params.items() if k != 'dispatch_probe'}
            self.rows.append((item.nodeid, params))

    def pytest_runtest_setup(self, item):
        self.current = item

    def pytest_sessionfinish(self, session):
        if session.exitstatus:
            return
        self.output.mkdir(parents=True, exist_ok=True)
        (self.output / 'reference.tsv').write_text(''.join(sorted(self.manifest)))
        (self.output / 'specs.json').write_text(json.dumps(self.specs, sort_keys=True))
        (self.output / 'rows.json').write_text(json.dumps(self.rows))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('output', type=Path)
    parser.add_argument('--native', type=Path, help='directory recorded with LH_GENERATOR_SPECS')
    args = parser.parse_args()
    os.chdir(ROOT)
    capture = Capture(args.output)
    status = pytest.main(['-q', 'tests/test_dispatch_index.py', 'tests/test_generator_parity.py'], plugins=[capture])
    if status:
        raise SystemExit(status)
    if args.native:
        native = {}
        for path in args.native.glob('*.json'):
            record = json.loads(path.read_text())
            native[record['key']] = canonical(record['spec'], record['temporary'])
        assert native == capture.specs, ('description mismatch', sorted(native.keys() ^ capture.specs.keys()),
            [k for k in native.keys() & capture.specs.keys() if native[k] != capture.specs[k]])
        print(f'{len(native)} native descriptions equal pytest descriptions byte for byte')


if __name__ == '__main__':
    main()
