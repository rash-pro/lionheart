"""Compile metadata only into the optional host FMV backend."""
import json
from pathlib import Path
import re
import sys

rows = json.loads(Path(sys.argv[1]).read_text())
lines = ['/* Generated from movies.json. No game bytes. */']
for key, row in rows.items():
    disc, lba = map(int, key.split(':'))
    if not re.fullmatch(r'disc\d\d_\d\d', row['movie']):
        raise ValueError('unsafe movie basename')
    lines.append('{%d,%d,%d,%d,"%s"},' % (disc, lba, row['frames'], row['offset_frames'], row['movie']))
Path(sys.argv[2]).write_text('\n'.join(lines) + '\n')
