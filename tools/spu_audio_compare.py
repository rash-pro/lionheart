"""Compare PCM16 stereo WAV excerpts without treating spectra as a tempo proof."""
import argparse
import json
from pathlib import Path
import wave

import numpy as np


def read(path, start, seconds):
    with wave.open(str(path)) as w:
        if w.getnchannels() != 2 or w.getsampwidth() != 2:
            raise ValueError('requires PCM16 stereo')
        rate = w.getframerate()
        w.setpos(min(round(start * rate), w.getnframes()))
        data = np.frombuffer(w.readframes(round(seconds * rate)), dtype='<i2').reshape(-1, 2).copy()
    if not len(data):
        raise ValueError(f'empty excerpt: {path}')
    return rate, data


def metrics(rate, data):
    stereo = data.astype(float)
    window = max(1, rate // 100)
    samples = len(stereo) // window * window
    envelope = np.sqrt(np.mean(stereo[:samples].reshape(-1, window, 2) ** 2, axis=(1, 2)))
    size = min(8192, len(stereo))
    bands = np.geomspace(30, 20000, 33)
    powers = np.zeros(32)
    for start in range(0, len(stereo) - size + 1, max(1, size // 2)):
        # Average channel powers so opposite-phase stereo remains audible.
        block = stereo[start:start+size] * np.hanning(size)[:, None]
        spectrum = np.mean(np.abs(np.fft.rfft(block, axis=0)) ** 2, axis=1)
        frequencies = np.fft.rfftfreq(size, 1 / rate)
        powers += np.array([spectrum[(frequencies >= low) & (frequencies < high)].sum()
                            for low, high in zip(bands[:-1], bands[1:])])
    total = powers.sum()
    return dict(seconds=len(data)/rate, rms=float(np.sqrt(np.mean(data.astype(float)**2))),
                peak=int(np.abs(data.astype(np.int32)).max()),
                silent_10ms_fraction=float(np.mean(envelope < 1)),
                nonzero_frames=int(np.count_nonzero(np.any(data != 0, axis=1))),
                band_power=(powers / total if total else powers).tolist(),
                envelope_10ms=envelope.tolist())


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('reference', type=Path)
    p.add_argument('native', type=Path)
    p.add_argument('--reference-start', type=float, default=0)
    p.add_argument('--native-start', type=float, default=0)
    p.add_argument('--seconds', type=float, default=8)
    p.add_argument('--output', type=Path, required=True)
    p.add_argument('--excerpts', type=Path)
    args = p.parse_args()
    result = {}
    for label, path, start in [('host', args.reference, args.reference_start), ('native', args.native, args.native_start)]:
        rate, data = read(path, start, args.seconds)
        result[label] = dict(path=str(path), start=start, rate=rate, **metrics(rate,data))
        if args.excerpts:
            args.excerpts.mkdir(parents=True, exist_ok=True)
            with wave.open(str(args.excerpts / f'{label}.wav'), 'wb') as w:
                w.setparams((2,2,rate,0,'NONE','not compressed')); w.writeframes(data.tobytes())
    a, b = (np.array(result[k]['band_power']) for k in ['host','native'])
    denominator = np.linalg.norm(a) * np.linalg.norm(b)
    result['spectral_cosine'] = float(a @ b / denominator) if denominator else None
    result['limitation'] = 'Spectra and levels do not establish event timing, tempo or absence of dropouts.'
    args.output.write_text(json.dumps(result,indent=2)+'\n')
    print(args.output)


if __name__ == '__main__':
    main()
