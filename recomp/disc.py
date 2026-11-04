"""Versioned USA disc extraction; binary assets stay in the build directory."""
import hashlib
import json
import struct
from pathlib import Path

from tools.ff8img import Disc, extract
from .discover import Image, discover

ROOT = Path(__file__).resolve().parents[1]
METADATA = ROOT / 'games/ff8-usa/overlays.json'
# Loader entry points documented in ARCHITECTURE.md.
ENTRIES = {0: 0x80098028, 1: 0x80098FD4, 2: 0x8009895C,
           23: 0x80098304, 24: 0x80098238, 25: 0x80099D30}


def main_image(path):
    image = Image.read(path)
    metadata = json.loads(METADATA.read_text())
    if hashlib.sha1(path.read_bytes()).hexdigest() != metadata['exe_sha1']:
        raise ValueError('unsupported EXE SHA-1; FF8 USA metadata required')
    image.code_ranges = tuple((int(lo, 16), int(hi, 16)) for lo, hi in metadata['main_code_ranges'])
    image.entry_points = tuple(int(pc, 16) for pc in metadata.get('main_entry_points', ()))
    image.timing_entry_points = tuple(int(pc, 16) for pc in metadata.get('timing_entry_points', ()))
    if any(not image.executable(pc) for pc in image.entry_points):
        raise ValueError('main entry point outside executable ranges')
    return image


def extract_disc(cue, work=None):
    metadata = json.loads(METADATA.read_text())
    work = Path(work) if work is not None else ROOT / 'build/extracted'
    work.mkdir(parents=True, exist_ok=True)
    disc = Disc(cue)
    try:
        files = {name.strip('/'): row for name, row in disc.walk()}
        matches = [(int(n), info) for n, info in metadata['discs'].items()
                   if info['exe'] in files and info['image'] in files]
        if len(matches) != 1:
            raise ValueError('disc does not identify one supported FF8 USA disc')
        number, info = matches[0]
        expected = {r['index']: r for r in info['overlays']}
        destination = work / 'overlays' / f'disc{number}'
        destination.mkdir(parents=True, exist_ok=True)
        record = files[info['exe']]
        exe = disc.read(record['lba'], record['size'])
        if hashlib.sha1(exe).hexdigest() != metadata['exe_sha1']:
            raise ValueError('unsupported disc EXE SHA-1')
        path = work / 'SLUS_008.92'
        path.write_bytes(exe)
        image = main_image(path)
        record = files[info['image']]
        table = disc.read(record['lba'], 2048)
        overlays = []
        for index, row in expected.items():
            if 'img_sector' in row:
                lba, size = record['lba'] + row['img_sector'], row['stored_size']
            else:
                lba, size = struct.unpack_from('<II', table, index * 8)
            data = extract(disc, lba, size, row['compressed'])
            base = int(row['load_address'], 16)
            facts = {'index': index, 'load_address': f"0x{base:08x}",
                     'size': len(data), 'sha1': hashlib.sha1(data).hexdigest()}
            if facts != {key: expected.get(index, {}).get(key) for key in facts}:
                raise ValueError(f'overlay {index} does not match versioned metadata')
            (destination / f'{index:02d}.bin').write_bytes(data)
            ov = Image(data, base, ENTRIES.get(index, base))
            # Prologues and entry tables anchor traversal, never calls decoded from image pixels.
            seeds = [ov.base + offset for offset in range(0, len(data) - 3, 4)
                     if struct.unpack_from('<I', data, offset)[0] >> 16 == 0x27BD
                     and struct.unpack_from('<h', data, offset)[0] < 0]
            functions = discover(ov, seeds, scan_calls=False)
            overlays.append(((number - 1) * 32 + index, ov, functions))
        return image, overlays
    finally:
        disc.bin.close()
