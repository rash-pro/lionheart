"""Inspect IROS v0x10002 and extract user-owned movies outside the checkout.

The directory layout was measured from cinema-rf.iroj, without third-party code.
Unknown compression flags fail closed. The supplied archive has only raw entries.
"""
import argparse
from dataclasses import dataclass
import json
import os
from pathlib import Path, PurePosixPath
import struct


@dataclass(frozen=True)
class Entry:
    name: str
    flags: int
    offset: int
    size: int


def directory(path):
    size = path.stat().st_size
    with path.open('rb') as stream:
        header = stream.read(16)
        if len(header) != 16:
            raise ValueError('truncated IROS header')
        magic, version, flags, at = struct.unpack('<4sIII', header)
        if magic != b'IROS' or version != 0x10002 or flags or not 16 <= at <= size - 4:
            raise ValueError('unsupported IROS header')
        stream.seek(at)
        count, = struct.unpack('<I', stream.read(4))
        if count > 100000:
            raise ValueError('invalid IROS entry count')
        entries = []
        for _ in range(count):
            start = stream.tell()
            header = stream.read(4)
            if len(header) != 4:
                raise ValueError('truncated IROS directory')
            length, name_length = struct.unpack('<HH', header)
            if name_length % 2 or length < name_length + 20 or start + length > size:
                raise ValueError('invalid IROS directory record')
            name = stream.read(name_length).decode('utf-16-le').replace('\\', '/')
            if not name or PurePosixPath(name).is_absolute() or '..' in PurePosixPath(name).parts or ':' in name:
                raise ValueError('unsafe IROS entry name')
            flags, offset, length_data = struct.unpack('<IQI', stream.read(16))
            if offset > size or length_data > size - offset:
                raise ValueError('IROS entry extends beyond archive')
            entries.append(Entry(name, flags, offset, length_data))
            stream.seek(start + length)
        end = stream.tell()
        if any(e.offset < end for e in entries):
            raise ValueError('IROS data overlaps directory')
        return entries


def extract(path, destination, entries):
    root = Path(__file__).resolve().parents[1]
    destination = destination.expanduser().resolve()
    if destination.is_relative_to(root):
        raise ValueError('extract user assets outside the repository')
    selected = [e for e in entries if e.name.lower().endswith(('.avi', '.cam'))]
    if any(e.flags for e in selected):
        raise ValueError('compressed IROS entries are unsupported; no files extracted')
    def relative(entry):
        parts = PurePosixPath(entry.name).parts
        if 'lang-en' in parts:
            return Path(*parts[parts.index('lang-en'):])
        return Path(parts[-1])
    names = [str(relative(e)).lower() for e in selected]
    if len(set(names)) != len(names):
        raise ValueError('duplicate movie basenames')
    destination.mkdir(parents=True, exist_ok=True)
    with path.open('rb') as stream:
        for entry in selected:
            target = destination / relative(entry)
            if not target.resolve().is_relative_to(destination):
                raise ValueError('destination escapes cache through a symlink')
            target.parent.mkdir(parents=True, exist_ok=True)
            if target.is_symlink():
                raise ValueError('refusing a symlink destination')
            if target.is_file() and target.stat().st_size == entry.size:
                stream.seek(entry.offset)
                with target.open('rb') as cached:
                    remaining, equal = entry.size, True
                    while remaining:
                        count = min(remaining, 1024 * 1024)
                        if stream.read(count) != cached.read(count):
                            equal = False
                            break
                        remaining -= count
                if equal:
                    continue
            temporary = target.with_name(target.name + '.part')
            if temporary.exists():
                raise ValueError(f'remove incomplete extraction first: {temporary}')
            stream.seek(entry.offset)
            try:
                with temporary.open('xb') as output:
                    remaining = entry.size
                    while remaining:
                        data = stream.read(min(remaining, 1024 * 1024))
                        if not data:
                            raise ValueError('truncated IROS data')
                        output.write(data)
                        remaining -= len(data)
                temporary.replace(target)
            finally:
                temporary.unlink(missing_ok=True)
    return len(selected)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('archive', type=Path)
    parser.add_argument('--list', action='store_true')
    parser.add_argument('--output', type=Path, default=Path(os.environ.get('XDG_CACHE_HOME') or Path.home() / '.cache') / 'ff8-recomp/movies/cinema-rf')
    args = parser.parse_args()
    entries = directory(args.archive)
    if args.list:
        print(json.dumps([e.__dict__ for e in entries], indent=2))
    else:
        print(f'Extracted {extract(args.archive, args.output, entries)} movies/cameras to {args.output}')


if __name__ == '__main__':
    main()
