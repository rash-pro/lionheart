#!/usr/bin/env python3
"""Generate a local catalog from owned USA discs, then build the developer host."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import time

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def version():
    sources = sorted([*ROOT.glob('recomp/*.py'), *ROOT.glob('games/ff8-usa/*.json'),
                      *(ROOT / 'tools' / name for name in ('setup.py', 'psxdisc.py', 'ff8files.py', 'ff8img.py'))])
    return hashlib.sha256(''.join(str(p.relative_to(ROOT)) + digest(p) for p in sources).encode()).hexdigest()


def run(args):
    print(' '.join(map(str, args)), flush=True)
    subprocess.run(list(map(str, args)), cwd=ROOT, check=True)


def setup(discs, jobs=8):
    from recomp.disc import extract_disc, main_image
    from recomp.discover import discover
    from recomp.emit import emit
    from tools.psxdisc import Disc
    start = time.monotonic()
    generated = ROOT / 'build/generated'
    extracted = generated / 'extracted'
    extracted.mkdir(parents=True, exist_ok=True)
    facts = json.loads((ROOT / 'games/ff8-usa/exe-identity.json').read_text())
    identities = json.loads((ROOT / 'games/ff8-usa/overlays.json').read_text())['discs']
    supplied = {}
    for path in discs:
        disc = Disc(path)
        try:
            names = {name.strip('/'): row for name, row in disc.walk()}
            matches = [int(n) for n, info in identities.items() if info['exe'] in names and info['image'] in names]
            if len(matches) != 1:
                raise ValueError(f'{path}: expected a supported USA disc')
            number = matches[0]
            row = names[identities[str(number)]['exe']]
            exe = disc.read(row['lba'], row['size'])
            if len(exe) != facts['file_size'] or hashlib.sha1(exe).hexdigest() != facts['sha1']:
                raise ValueError(f'{path}: unsupported EXE identity')
            if number in supplied:
                raise ValueError(f'duplicate disc {number}')
            supplied[number] = path
        finally:
            disc.bin.close()
    if 1 not in supplied:
        raise ValueError('disc 1 is required')
    (extracted / 'SLUS_008.92').write_bytes(exe)
    key = {'exe_sha1': facts['sha1'], 'recompiler_version': version(), 'discs': sorted(supplied)}
    cache_path = generated / 'setup-cache.json'
    cache = json.loads(cache_path.read_text()) if cache_path.exists() else {}
    hit = (cache.get('key') == key and bool(cache.get('files')) and
           all((generated / p).is_file() and digest(generated / p) == value for p, value in cache.get('files', {}).items()))
    generation_start = time.monotonic()
    if hit:
        print('Catalog cache hit: verified EXE, recompiler version, disc set and generated files.', flush=True)
    else:
        print('Catalog cache miss: generating exact native functions.', flush=True)
        overlays = []
        for number, path in sorted(supplied.items()):
            print(f'Extracting and discovering disc {number} ({len(overlays)} overlay sets ready)...', flush=True)
            image, current = extract_disc(path, extracted)
            overlays.extend(current)
        image = main_image(extracted / 'SLUS_008.92')
        print(f'Discovering main executable and emitting {len(overlays)} overlay sets...', flush=True)
        emit(image, discover(image), generated, overlays=overlays, mode='fast', host_mode='precise')
        outputs = {p.name: digest(p) for p in generated.iterdir() if p.is_file() and p.name != cache_path.name}
        cache_path.write_text(json.dumps({'key': key, 'files': outputs}, indent=2) + '\n')
    generation_seconds = time.monotonic() - generation_start
    run(['cmake', '-S', ROOT, '-B', ROOT / 'build/dev', '-DCMAKE_BUILD_TYPE=Release',
         '-DFF8_PLAYER=OFF', f'-DGENERATED_DIR={generated}', '-DFF8_GUI=ON', '-DFF8_NFD=ON',
         '-DFF8_VULKAN=ON', '-DFF8_PGXP=ON', '-DFF8_STATS=ON', '-DFF8_MOVIES=ON',
         '-DFF8_RCHEEVOS=ON', '-DFF8_STATE_BRIDGE=ON', '-DFF8_SAVESTATES=ON'])
    run(['cmake', '--build', ROOT / 'build/dev', '-j', jobs])
    receipt = {'cache_hit': hit, 'catalog_seconds': generation_seconds,
               'total_seconds': time.monotonic() - start, 'key': key}
    (ROOT / 'build/setup-timing.json').write_text(json.dumps(receipt, indent=2) + '\n')
    print(f"Setup complete in {receipt['total_seconds']:.2f}s (catalog {generation_seconds:.2f}s).")
    command = [str(ROOT / 'build/dev/ff8'), str(extracted / 'SLUS_008.92'), '--gui']
    for path in supplied.values(): command += ['--disc', str(path)]
    import shlex
    print('Run: ' + shlex.join(command))
    return receipt


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for n in range(1, 5): parser.add_argument(f'--disc{n}', type=Path, required=n == 1)
    parser.add_argument('--jobs', type=int, choices=range(1, 9), default=8)
    args = parser.parse_args()
    try:
        setup([p.resolve() for n in range(1, 5) if (p := getattr(args, f'disc{n}'))], args.jobs)
    except (OSError, ValueError, subprocess.CalledProcessError) as error:
        parser.exit(1, f'setup: {error}\n')


if __name__ == '__main__': main()
