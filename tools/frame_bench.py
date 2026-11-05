"""Alternate two frame-probe hosts on identical copied cards, without captures.

Each receipt records Linux load, binary hashes, completion, guest work, and both
CPU-thread and elapsed-frame distributions. Subsystem probes are diagnostic only.
Use --first to exclude boot, pipeline warmup, and loading from the timed window.
"""
import argparse
import csv
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import statistics
import subprocess
import time


def sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def distribution(values):
    values = sorted(values)
    if not values:
        raise ValueError('empty timing window')
    result = {f'p{p}': values[round((len(values)-1)*p/100)] for p in (50, 95, 99)}
    return {**result, 'max': values[-1], 'above_16_7': sum(v > 16.7 for v in values),
            'above_33': sum(v > 33 for v in values)}


def measure(binary, args, directory, checkpoint=None):
    directory.mkdir(parents=True, exist_ok=False)
    card = directory / 'card.mcr'
    original = sha(args.card)
    shutil.copyfile(args.card, card)
    config = directory / 'empty.ini'
    config.write_text('')
    command = [str(binary.resolve()), str(args.exe.resolve()), '--config', str(config),
               '--headless', '--fast', '--mute', '--renderer', args.renderer,
               '--stall', '0', '--max-instructions', '0', '--max-vblanks', str(args.last),
               '--input-script', str(args.script.resolve()), '--card1', str(card),
               '--savestate-dir', str(directory / 'states'), '--screenshot-dir', str(directory / 'shots'),
               '--log', os.devnull]
    for disc in args.disc:
        command += ['--disc', str(disc.resolve())]
    if args.interpreter:
        command.append('--interpreter')
    if checkpoint:
        command += ['--load-state', str(checkpoint.resolve())]
    env = {k: v for k, v in os.environ.items() if not k.startswith('FF8_')}
    env.update(SDL_VIDEODRIVER='dummy', SDL_AUDIODRIVER='dummy',
               XDG_CONFIG_HOME=str(directory / 'config'), XDG_DATA_HOME=str(directory / 'data'),
               FF8_FRAME_TIMINGS=str(directory / 'frames.csv'))
    # The original local probe used this spelling; keep old baseline artifacts usable.
    env['BATTLE_TIMINGS'] = env['FF8_FRAME_TIMINGS']
    receipt = dict(binary_sha256=sha(binary), source_card_sha256=original,
                   script_sha256=sha(args.script), renderer=args.renderer,
                   interpreter=args.interpreter, first=args.first, last=args.last,
                   load_before=Path('/proc/loadavg').read_text().strip())
    if checkpoint:
        receipt['checkpoint_sha256'] = sha(checkpoint)
    start = time.monotonic()
    with (directory / 'run.log').open('w') as log:
        result = subprocess.run(command, env=env, stdout=log, stderr=subprocess.STDOUT)
    receipt.update(elapsed_seconds=time.monotonic()-start, exit_code=result.returncode,
                   load_after=Path('/proc/loadavg').read_text().strip())
    log = (directory / 'run.log').read_text()
    receipt['complete'] = result.returncode == 0 and 'Stop: VBlank limit reached' in log and f'VBlanks={args.last} ' in log
    receipt['source_card_unchanged'] = sha(args.card) == original
    if not receipt['complete'] or not receipt['source_card_unchanged']:
        (directory / 'receipt.json').write_text(json.dumps(receipt, indent=2)+'\n')
        raise ValueError(f'incomplete or modified input: {directory}')
    if args.renderer == 'vulkan':
        device = re.search(r'^Vulkan: (.+), scale=', log, re.MULTILINE)
        if not device or 'falling back to soft' in log:
            raise ValueError(f'requested Vulkan renderer was not used: {directory}')
        receipt['vulkan_device'] = device[1]
    rows = list(csv.DictReader((directory / 'frames.csv').open()))
    start_frame = int(rows[0]['vblank']) if checkpoint and rows else 1
    if (checkpoint and start_frame >= args.first) or [int(r['vblank']) for r in rows] != list(range(start_frame, args.last+1)):
        raise ValueError('missing or repeated VBlank samples')
    rows = [r for r in rows if int(r['vblank']) >= args.first]
    work = [[int(r[k]) for k in ('vblank', 'cycles', 'interpreted', 'compiled', 'idle', 'gte', 'cd_bytes')] for r in rows]
    receipt['work_sha256'] = hashlib.sha256(json.dumps(work).encode()).hexdigest()
    receipt['cycles'] = int(re.search(r'Hardware: cycles=(\d+)', log)[1])
    receipt['frames'] = len(rows)
    for clock in ('cpu_ms', 'wall_ms'):
        receipt[clock] = distribution([float(r[clock]) for r in rows])
    (directory / 'receipt.json').write_text(json.dumps(receipt, indent=2)+'\n')
    return receipt


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('before', 'after', 'exe', 'script', 'card', 'output'):
        parser.add_argument('--'+name, type=Path, required=True)
    parser.add_argument('--disc', type=Path, action='append', required=True)
    parser.add_argument('--renderer', choices=['soft', 'vulkan'], default='soft')
    parser.add_argument('--interpreter', action='store_true')
    parser.add_argument('--before-state', type=Path)
    parser.add_argument('--after-state', type=Path)
    parser.add_argument('--first', type=int, default=2100)
    parser.add_argument('--last', type=int, default=5400)
    parser.add_argument('--runs', type=int, default=5)
    args = parser.parse_args()
    if not 1 <= args.first <= args.last or args.runs < 1:
        parser.error('require 1 <= first <= last and at least one pair')
    args.output = args.output.resolve()
    receipts = {'before': [], 'after': []}
    for repeat in range(args.runs):
        for side in receipts:
            receipt = measure(getattr(args, side), args, args.output / f'{repeat:02d}-{side}',
                              getattr(args, side+'_state'))
            receipts[side].append(receipt)
            print(json.dumps(dict(repeat=repeat, side=side, **receipt)), flush=True)
            if len({r['work_sha256'] for values in receipts.values() for r in values}) != 1:
                raise ValueError('guest work changed between runs')
    summary = {'runs_per_side': args.runs, 'matched_work': True, 'results': {}}
    for side, runs in receipts.items():
        summary['results'][side] = {
            clock: {key: dict(median=statistics.median(r[clock][key] for r in runs),
                              min=min(r[clock][key] for r in runs), max=max(r[clock][key] for r in runs))
                    for key in runs[0][clock]} for clock in ('cpu_ms', 'wall_ms')}
    (args.output / 'summary.json').write_text(json.dumps(summary, indent=2)+'\n')


if __name__ == '__main__':
    main()
