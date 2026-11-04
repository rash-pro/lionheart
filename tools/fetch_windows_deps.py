"""Fetch only the locked official artifacts; verify before extracting, never vendor binaries."""
import argparse
import hashlib
import json
from pathlib import Path
import tarfile
import urllib.request

ROOT = Path(__file__).resolve().parents[1]
DEFAULT = ROOT / 'build/windows-deps'


def fetch(root=DEFAULT, cache=ROOT / 'build/downloads'):
    cache.mkdir(parents=True, exist_ok=True)
    paths = {}
    for dep in json.loads((ROOT / 'tools/windows-deps.json').read_text()):
        dest = root / f"{dep['name']}-{dep['version']}"
        source = json.dumps(dep, indent=2) + '\n'
        if (dest / 'SOURCE.md').is_file():
            if (dest / 'SOURCE.md').read_text() != source:
                raise RuntimeError(f'Different provenance at {dest}')
            paths[dep['name']] = dest
            continue
        archive = cache / dep['url'].rsplit('/', 1)[1]
        if not archive.exists():
            temp = archive.with_suffix('.partial')
            urllib.request.urlretrieve(dep['url'], temp)
            temp.replace(archive)
        digest = hashlib.sha256(archive.read_bytes()).hexdigest()
        if digest != dep['sha256'] or archive.stat().st_size != dep['size']:
            raise RuntimeError(f'Artifact identity mismatch: {archive}')
        dest = root / f"{dep['name']}-{dep['version']}"
        source = json.dumps(dep, indent=2) + '\n'
        if not (dest / 'SOURCE.md').exists():
            dest.mkdir(parents=True, exist_ok=True)
            with tarfile.open(archive) as tar:
                # Official tarballs have one enclosing directory.
                members = tar.getmembers()
                top = members[0].name.split('/')[0]
                for member in members:
                    member.name = member.name.removeprefix(top).lstrip('/')
                tar.extractall(dest, members=[m for m in members if m.name], filter='data')
            (dest / 'SOURCE.md').write_text(source)
        elif (dest / 'SOURCE.md').read_text() != source:
            raise RuntimeError(f'Different provenance at {dest}')
        paths[dep['name']] = dest
    return paths


if __name__ == '__main__':
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--root', type=Path, default=DEFAULT)
    p.add_argument('--cache', type=Path, default=ROOT / 'build/downloads')
    args = p.parse_args()
    for name, path in fetch(args.root, args.cache).items():
        print(f'{name}: {path}')
