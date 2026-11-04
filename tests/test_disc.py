"""Disc extraction can keep a shared image tree entirely read-only."""
import hashlib
import json
from pathlib import Path
import struct

import pytest

from recomp import disc as extraction


@pytest.mark.parametrize('external', [False, True])
def test_assets_directory_and_overlay_source(tmp_path, monkeypatch, external):
    exe, payload = b'owned EXE', bytes(32)
    index = 27 if external else 2
    table = bytearray(2048)
    struct.pack_into('<II', table, index * 8, 77, len(payload))
    row = dict(index=index, name='movie' if external else 'field', load_address='0x80098000',
               compressed=False, size=len(payload), sha1=hashlib.sha1(payload).hexdigest())
    if external:
        row.update(img_sector=88, stored_size=len(payload))
    metadata = tmp_path / 'metadata.json'
    metadata.write_text(json.dumps(dict(exe_sha1=hashlib.sha1(exe).hexdigest(),
        discs={'1': dict(exe='SLUS_008.92', image='FF8DISC1.IMG', overlays=[row])})))
    reads = []

    class Disc:
        def __init__(self, cue):
            self.bin = (tmp_path / 'closed').open('wb')

        def walk(self):
            return [('SLUS_008.92', dict(lba=10, size=len(exe))),
                    ('FF8DISC1.IMG', dict(lba=20, size=2048))]

        def read(self, sector, size):
            reads.append((sector, size))
            return {10: exe, 20: bytes(table), 77: payload, 108: payload}[sector]

    monkeypatch.setattr(extraction, 'Disc', Disc)
    monkeypatch.setattr(extraction, 'METADATA', metadata)
    monkeypatch.setattr(extraction, 'ROOT', tmp_path / 'shared')
    monkeypatch.setattr(extraction, 'main_image', lambda path: path.read_bytes())
    monkeypatch.setattr(extraction, 'discover', lambda *args, **kwargs: {})
    assets = tmp_path / 'build/assets'
    image, overlays = extraction.extract_disc(Path('disc.cue'), assets)
    assert image == exe and overlays[0][0] == index
    assert (assets / 'SLUS_008.92').read_bytes() == exe
    assert (assets / f'overlays/disc1/{index:02d}.bin').read_bytes() == payload
    assert not (tmp_path / 'shared/work').exists()
    assert reads[-1] == (108 if external else 77, len(payload))


def test_movie_metadata_matches_existing_catalog():
    root = Path(__file__).resolve().parents[1]
    metadata = json.loads((root / 'games/ff8-usa/overlays.json').read_text())
    catalog = json.loads((root / 'games/ff8-usa/overlays-all.json').read_text())
    movie = next(row for row in catalog['overlays'] if row['name'] == 'movie')
    for location in movie['locations']:
        row = next(row for row in metadata['discs'][str(location['disc'])]['overlays'] if row['index'] == 27)
        assert (row['load_address'], row['sha1'], row['size'], row['img_sector'], row['stored_size']) == (
            movie['load_address'], movie['sha1'], movie['size'], location['sector'], location['read_size'])
