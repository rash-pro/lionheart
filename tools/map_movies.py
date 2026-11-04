"""Match SMJ streams on owned PS1 discs to owned PC AVIs using decoded images.

Only the resulting LBA/name/frame/offset metadata belongs in git. Decoded images,
per-candidate scores and ffprobe facts stay in --evidence under build/.
"""
import argparse
from concurrent.futures import ThreadPoolExecutor
import json
from pathlib import Path
import struct
import subprocess
import sys

import numpy as np

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tests'))
from verify_movie_frames import chunk  # Original RIFF writer, no game bytes.


def scan(path):
    raw = np.memmap(path, dtype=np.uint8, mode='r', shape=(path.stat().st_size // 2352, 2352))
    h = raw[:, 24:32]
    starts = np.flatnonzero((h[:, :4] == [83, 77, 74, 1]).all(axis=1) &
                           (h[:, 4:] == [2, 9, 0, 0]).all(axis=1)) - 2
    result = []
    for start in starts:
        headers = h[start + 2::10][:10000]
        frame = headers[:, 6].astype(np.uint32) + headers[:, 7].astype(np.uint32) * 256
        valid = ((headers[:, :6] == [83, 77, 74, 1, 2, 9]).all(axis=1) &
                 (frame == np.arange(len(frame))))
        bad = np.flatnonzero(~valid)
        count = int(bad[0]) if len(bad) else len(valid)
        result.append((int(start), count))
    return result


def sample_avi(path, lba, indices, output):
    width, height, count = 320, 224, len(indices)
    avih = chunk(b'avih', struct.pack('<14I', 66667, 0, 0, 0, count, 0, 1, 0, width, height, 0, 0, 0, 0))
    strh = chunk(b'strh', b'vidsMDEC' + struct.pack('<IHHIIIIIIIIhhhh',
                 0, 0, 0, 0, 1, 15, 0, count, 0, 0xffffffff, 0, 0, 0, width, height))
    strf = chunk(b'strf', struct.pack('<IiiHH4sIiiII', 40, width, height, 1, 24, b'MDEC', width * height * 3, 0, 0, 0, 0))
    packets = []
    with path.open('rb') as stream:
        for index in indices:
            pieces = []
            for sector in range(2, 10):
                stream.seek((lba + index * 10 + sector) * 2352 + 24)
                data = stream.read(2048)
                if data[:8] != b'SMJ\1' + bytes([sector, 9]) + struct.pack('<H', index):
                    raise ValueError('SMJ sample header mismatch')
                pieces.append(data[8:])
            packets.append(chunk(b'00dc', b''.join(pieces)))
    listing = lambda tag, data: chunk(b'LIST', tag + data)
    output.write_bytes(chunk(b'RIFF', b'AVI ' + listing(b'hdrl', avih + listing(b'strl', strh + strf)) + listing(b'movi', b''.join(packets))))


def decode(path, codec=None):
    command = ['ffmpeg', '-nostdin', '-v', 'error', '-threads', '1']
    if codec:
        command += ['-c:v', codec]
    command += ['-i', str(path), '-an', '-vf', 'scale=40:28:flags=area,format=gray',
                '-threads', '1', '-fps_mode', 'passthrough', '-f', 'rawvideo', '-']
    return np.frombuffer(subprocess.check_output(command), np.uint8).reshape(-1, 28, 40)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--movies', type=Path, required=True)
    parser.add_argument('--extra-movies', type=Path, action='append', default=[])
    parser.add_argument('--disc', type=Path, action='append', required=True, help='raw MODE2/2352 BIN in disc order')
    parser.add_argument('--output', type=Path, default=ROOT / 'games/ff8-usa/movies.json')
    parser.add_argument('--evidence', type=Path, default=ROOT / 'build/host/mapping')
    args = parser.parse_args()
    if not args.evidence.resolve().is_relative_to(ROOT / 'build'):
        parser.error('decoded evidence must stay under build/')
    args.evidence.mkdir(parents=True, exist_ok=True)
    paths = sorted(args.movies.glob('disc??_??h.avi'))
    for directory in args.extra_movies:
        paths += sorted(directory.glob('disc??_??h.avi'))
    def pc(path):
        cache = args.evidence / (path.stem + '.npy')
        if cache.exists():
            images = np.load(cache)
        else:
            images = decode(path)
            np.save(cache, images)
        info = json.loads(subprocess.check_output(['ffprobe', '-v', 'error', '-select_streams', 'v:0', '-show_streams', '-of', 'json', str(path)]))['streams'][0]
        fps = info['avg_frame_rate']
        a, b = map(int, fps.split('/'))
        return dict(movie=path.stem[:-1], images=images, fps=a / b, fps_ratio=fps,
                    width=info['width'], height=info['height'], frames=len(images))
    with ThreadPoolExecutor(max_workers=4) as pool:
        movies = list(pool.map(pc, paths))
    mapping, evidence = {}, {}
    for disc, path in enumerate(args.disc, 1):
        for lba, count in scan(path):
            indices = sorted(set([0, min(5, count-1)] + [min(count-1, int(count * f)) for f in (.1, .25, .5, .75, .9)]))
            avi = args.evidence / f'{disc}-{lba}.avi'
            sample_avi(path, lba, indices, avi)
            original = decode(avi, 'mdec').astype(np.int16)
            candidates = []
            for movie in movies:
                # A PC asset can omit a final still or include a short leader.
                if abs(movie['frames'] / movie['fps'] - count / 15) > max(1.0, count / 15 * .08):
                    continue
                for offset in range(-90, 91):
                    targets = np.clip(np.rint((np.array(indices) / 15 + offset / 15) * movie['fps']).astype(int), 0, movie['frames']-1)
                    sample = movie['images'][targets].astype(np.int16)
                    error = float(np.abs(sample - original).mean())
                    candidates.append((error, movie['movie'], offset, movie))
            if not candidates:
                raise ValueError(f'no duration candidate for {disc}:{lba}')
            candidates.sort(key=lambda row: (row[0], abs(row[2]), row[1]))
            error, name, offset, movie = candidates[0]
            others = [c for c in candidates if c[1] != name]
            row = dict(movie=name, frames=count, pc_frames=movie['frames'], pc_fps=movie['fps_ratio'], offset_frames=offset)
            mapping[f'{disc}:{lba}'] = row
            evidence[f'{disc}:{lba}'] = dict(**row, sample_frames=indices, mean_absolute_error=error,
                runner_up=[dict(error=c[0], movie=c[1], offset=c[2]) for c in others[:3]])
            print(disc, lba, name, count, movie['frames'], offset, round(error, 3), flush=True)
            avi.unlink()
    args.output.write_text(json.dumps(mapping, indent=2) + '\n')
    (args.evidence / 'matches.json').write_text(json.dumps(evidence, indent=2) + '\n')


if __name__ == '__main__':
    main()
