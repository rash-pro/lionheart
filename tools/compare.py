#!/usr/bin/env python3
"""Compare complete guest dumps and every native software PNG from two builds."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess


def sha(path): return hashlib.sha256(path.read_bytes()).hexdigest()


def run(binary, exe, discs, script, count, dest, card=None, launcher=(), extra_args=()):
    dest = Path(dest).resolve()
    dest.mkdir(parents=True, exist_ok=True)
    config = dest / 'empty.ini'; config.write_text('')
    private_card = dest / 'card.mcr'
    if card and Path(card).resolve() == private_card.resolve():
        raise ValueError('input card must be outside the comparison output directory')
    if private_card.exists(): private_card.unlink()
    original = sha(card) if card else None
    if card: shutil.copyfile(card, private_card)
    command = [*map(str, launcher), str(Path(binary).resolve()), str(Path(exe).resolve()),
               '--config', str(config), '--headless', '--fast', '--mute', '--renderer', 'soft',
               '--stall', '0', '--max-instructions', '0', '--max-vblanks', str(count),
               '--input-script', str(Path(script).resolve()), '--card1', str(private_card),
               '--savestate-dir', str(dest / 'states'), '--screenshot-dir', str(dest / 'shots'),
               '--dump-state', str(dest / 'state.bin'), '--dump-frames', str(dest / 'frames'),
               '--frame-step', '1', '--log', 'NUL' if launcher else os.devnull]
    for disc in discs: command += ['--disc', str(Path(disc).resolve())]
    command += list(extra_args)
    env = {**os.environ, 'SDL_VIDEODRIVER': 'dummy', 'SDL_AUDIODRIVER': 'dummy',
           'XDG_CONFIG_HOME': str(dest / 'config'), 'XDG_DATA_HOME': str(dest / 'data')}
    with (dest / 'run.log').open('w') as log:
        result = subprocess.run(command, stdout=log, stderr=subprocess.STDOUT, env=env, cwd=dest)
    text = (dest / 'run.log').read_text()
    if result.returncode or 'Stop: VBlank limit reached' not in text or f'VBlanks={count} ' not in text:
        raise ValueError(f'incomplete run: {dest}/run.log')
    frames = sorted((dest / 'frames').glob('*.png'))
    if len(frames) != count: raise ValueError(f'expected {count} frames, got {len(frames)}')
    receipt = {'command': command, 'binary_sha256': sha(Path(binary)), 'state': sha(dest / 'state.bin'),
               'frames': {p.name: sha(p) for p in frames}, 'card': sha(private_card),
               'source_card_unchanged': not card or sha(card) == original,
               'cycles': int(re.search(r'Hardware: cycles=(\d+)', text)[1])}
    if not receipt['source_card_unchanged']: raise ValueError('input memory card changed')
    (dest / 'receipt.json').write_text(json.dumps(receipt, indent=2) + '\n')
    for frame in frames[:-1]: frame.unlink()
    return receipt


def compare(before, after):
    keys = ['state', 'frames', 'card', 'cycles']
    result = {key: before[key] == after[key] for key in keys}
    if not all(result.values()): raise ValueError(f'exactness mismatch: {result}')
    return {'passed': True, **result, 'pngs': len(before['frames'])}


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--before', type=Path, required=True)
    p.add_argument('--after', type=Path, required=True)
    p.add_argument('--exe', type=Path, required=True)
    p.add_argument('--disc', type=Path, action='append', required=True)
    p.add_argument('--script', type=Path, default=Path('tests/scripts/new-game.txt'))
    p.add_argument('--vblanks', type=int, default=15000)
    p.add_argument('--card', type=Path)
    p.add_argument('--output', type=Path, default=Path('build/exactness'))
    p.add_argument('--before-arg', action='append', default=[], help='extra reference-player argument (repeatable)')
    p.add_argument('--after-arg', action='append', default=[], help='extra comparison-player argument (repeatable)')
    a = p.parse_args()
    receipts = [run(binary, a.exe, a.disc, a.script, a.vblanks, a.output / label, a.card, extra_args=args)
                for label, binary, args in [('before', a.before, a.before_arg), ('after', a.after, a.after_arg)]]
    result = compare(*receipts)
    (a.output / 'comparison.json').write_text(json.dumps(result, indent=2) + '\n')
    print(json.dumps(result))


if __name__ == '__main__': main()
