"""Summarize cumulative CPU diagnostics over exact VBlank boundaries.

Example: python tools/perf_report.py --log build/profile/world.txt --first 1800 \
    --last 2400 --histogram build/profile/world-pcs.csv --output build/profile/world.json
Cross-page transfers mean guest 4 KiB pages. Memory rows count c_memory sites;
RAM/scratch/MMIO/other are exclusive, gap/fence/watched are overlapping reasons.
A backedge is only a candidate, never a proof of an invariant poll loop.
"""
import argparse
import csv
import json
from pathlib import Path
import re


def summarize(log, first, last, histogram=None):
    counters, candidates = {}, {}
    for line in log.splitlines():
        position = line.find('CPU ')
        if position >= 0:
            line = line[position:]
        if not line.startswith(('CPU MEMORY ', 'CPU DISPATCH ', 'CPU CANDIDATE ')):
            continue
        fields = dict(re.findall(r'(\w+)=(\w+)', line))
        vb = int(fields.pop('vb'))
        if vb not in (first, last):
            continue
        if line.startswith('CPU CANDIDATE '):
            key = fields['pc'], int(fields['kind'])
            candidates.setdefault(vb, {})[key] = int(fields['hits'])
        else:
            group = line.split()[1].lower()
            counters.setdefault(vb, {}).update({group + '.' + k: int(v) for k, v in fields.items()})
    if first not in counters or last not in counters:
        raise ValueError('missing exact boundary snapshots')
    delta = {k: v - counters[first][k] for k, v in counters[last].items()}
    if any(v < 0 for v in delta.values()):
        raise ValueError('counters decreased')
    hits = [(pc, kind, n - candidates.get(first, {}).get((pc, kind), 0))
            for (pc, kind), n in candidates.get(last, {}).items()]
    result = dict(first_vblank=first, last_vblank=last, counters=delta,
        candidates_complete=not delta['dispatch.candidate_lost'],
        candidates=[dict(pc=pc, kind=kind, hits=n) for pc, kind, n in sorted(hits, key=lambda r: -r[2]) if n])
    if histogram:
        rows = {int(row['pc'], 16): int(row['instructions']) for row in csv.DictReader(histogram.open())}
        total = sum(rows.values())
        if not total:
            raise ValueError('empty PC histogram')
        battle = [*range(0x800d073c, 0x800d074c, 4), *range(0x800cf3fc, 0x800cf40c, 4)]
        result['instructions'] = total
        result['battle_poll'] = dict(instructions=sum(rows.get(pc, 0) for pc in battle),
            percent=100 * sum(rows.get(pc, 0) for pc in battle) / total,
            pcs={f'{pc:08x}': rows.get(pc, 0) for pc in battle})
        result['world_bf374'] = rows.get(0x800bf374, 0)
        result['hot_pcs'] = [dict(pc=f'{pc:08x}', instructions=n, percent=100*n/total)
                             for pc, n in sorted(rows.items(), key=lambda r: -r[1])[:40]]
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--log', type=Path, required=True)
    parser.add_argument('--first', type=int, required=True)
    parser.add_argument('--last', type=int, required=True)
    parser.add_argument('--histogram', type=Path)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    if args.first >= args.last:
        parser.error('first must precede last')
    result = summarize(args.log.read_text(), args.first, args.last, args.histogram)
    args.output.write_text(json.dumps(result, indent=2) + '\n')
    print(json.dumps({k: v for k, v in result.items() if k not in ('candidates', 'hot_pcs')}, indent=2))


if __name__ == '__main__':
    main()
