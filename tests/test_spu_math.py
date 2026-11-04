"""Mixer coefficient bounds and negative fixed-point rounding stay exact."""
import ctypes
from pathlib import Path
import random
import subprocess

import pytest


@pytest.fixture(scope="module")
def decoder(tmp_path_factory):
    directory = tmp_path_factory.mktemp('spu-decoder')
    source = directory / 'decode.c'
    source.write_text('#include "' + str(Path('runtime/spu.c').resolve()) +
                      '"\nvoid decode(const uint8_t *data, int16_t *history, int16_t *out) '
                      '{ decode_block(data, history, out); }\n')
    binary = directory / 'decode.so'
    subprocess.run(['cc', '-O2', '-shared', '-fPIC', str(source), '-o', str(binary)], check=True)
    lib = ctypes.CDLL(str(binary))
    lib.decode.argtypes = [ctypes.POINTER(ctypes.c_uint8), ctypes.POINTER(ctypes.c_int16),
                           ctypes.POINTER(ctypes.c_int16)]
    return lib.decode


def test_adpcm_matches_integer_reference(decoder):
    rng = random.Random(0xadc0)
    filters = [(0, 0), (60, 0), (115, -52), (98, -55), (122, -60)]
    for header in range(256):
        for payload in (bytes(14), bytes([0x88]) * 14, bytes([0x77]) * 14,
                        rng.randbytes(14)):
            for initial in ((0, 0), (-32768, 32767), (32767, -32768), (1, -1)):
                data = bytes([header, 0]) + payload
                history = (ctypes.c_int16 * 2)(*initial)
                out = (ctypes.c_int16 * 28)()
                decoder((ctypes.c_uint8 * 16).from_buffer_copy(data), history, out)
                shift, predictor = header & 15, header >> 4
                shift = 9 if shift > 12 else shift
                predictor = 0 if predictor > 4 else predictor
                a, b = filters[predictor]
                h0, h1 = initial
                expected = []
                for i in range(28):
                    nibble = (payload[i // 2] >> ((i & 1) * 4)) & 15
                    nibble = nibble - 16 if nibble & 8 else nibble
                    value = (nibble * 4096 >> shift) + ((h0 * a + h1 * b + 32) >> 6)
                    value = max(-32768, min(32767, value))
                    expected.append(value)
                    h0, h1 = value, h0
                assert list(out) == expected, (header, payload, initial)
                assert list(history) == [h0, h1]


def test_pcm_multiply_matches_wide_reference(tmp_path):
    source = tmp_path / 'mix.c'
    source.write_text('#include "' + str(Path('runtime/spu.c').resolve()) +
                      '"\nint32_t coefficient(int32_t a, int32_t b) { return mul(a,b); }\n'
                      'int32_t pcm(int32_t a, int32_t b) { return mul_sample(a,b); }\n')
    binary = tmp_path / 'mix.so'
    subprocess.run(['cc', '-O2', '-shared', '-fPIC', str(source), '-o', str(binary)], check=True)
    lib = ctypes.CDLL(str(binary))
    lib.coefficient.argtypes = [ctypes.c_int32, ctypes.c_int32]
    lib.coefficient.restype = ctypes.c_int32
    lib.pcm.argtypes = [ctypes.c_int32, ctypes.c_int32]
    lib.pcm.restype = ctypes.c_int32
    rng = random.Random(8)
    pairs = [(a,b) for a in [-98304,-65536,-32768,-1,0,1,32767,65535,98304]
             for b in [-32768,-1,0,1,32767]]
    pairs += [(a,b) for a in [-32768,-1,0,1,32767] for b in [32768,65534,65535]]
    pairs += [(rng.randrange(-98304,98305),rng.randrange(-32768,32768)) for _ in range(10000)]
    pairs += [(rng.randrange(-32768,32768),rng.randrange(65536)) for _ in range(10000)]
    for a,b in pairs:
        assert lib.coefficient(a,b) == (a*b) >> 15, (a,b)
    # Voice multiplication can produce +32768; pitch modulation can reach
    # 65536, but its other operand is explicitly int16_t.
    pcm = [(a, b) for a in [-32768, -1, 0, 1, 32767, 32768]
           for b in [-32768, -1, 0, 1, 32767]]
    pcm += [(a, b) for a in [-32768, -1, 0, 1, 32767] for b in [32768, 65535, 65536]]
    pcm += [(rng.randrange(-32768, 32769), rng.randrange(-32768, 32768)) for _ in range(10000)]
    pcm += [(rng.randrange(-32768, 32768), rng.randrange(65537)) for _ in range(10000)]
    for a, b in pcm:
        assert lib.pcm(a, b) == (a*b) >> 15, (a,b)
