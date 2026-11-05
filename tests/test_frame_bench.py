"""Timing reports reject incomplete work and count threshold crossings exactly."""
from pathlib import Path
from types import SimpleNamespace

import pytest

from tools.frame_bench import distribution, measure


def test_frame_distribution():
    result = distribution([1, 16.7, 16.8, 33, 33.1])
    assert result == dict(p50=16.8, p95=33.1, p99=33.1, max=33.1, above_16_7=3, above_33=1)
    with pytest.raises(ValueError, match='empty'):
        distribution([])


@pytest.mark.parametrize('complete', [True, False])
def test_frame_receipt_requires_every_vblank(tmp_path, complete):
    binary = tmp_path / 'host'
    binary.write_text('''#!/usr/bin/env python3
import os
from pathlib import Path
rows = 'vblank,wall_ms,cpu_ms,cycles,interpreted,compiled,idle,gte,cd_bytes\\n'
for n in range(1, END): rows += f'{n},2,1,10,5,0,0,0,0\\n'
Path(os.environ['FF8_FRAME_TIMINGS']).write_text(rows)
print('Stop: VBlank limit reached\\nHardware: cycles=20 VBlanks=2 ')
'''.replace('END', '3' if complete else '2'))
    binary.chmod(0o755)
    card = tmp_path / 'source.card'; card.write_bytes(bytes(128))
    script = tmp_path / 'route.txt'; script.write_text('1 -\n')
    args = SimpleNamespace(card=card, exe=binary, script=script, renderer='soft',
                           last=2, first=1, disc=[], interpreter=False)
    if complete:
        receipt = measure(binary, args, tmp_path / 'out')
        assert receipt['complete'] and receipt['source_card_unchanged']
        assert receipt['frames'] == 2 and receipt['cycles'] == 20
        assert receipt['load_before'] and receipt['load_after']
    else:
        with pytest.raises(ValueError, match='missing or repeated'):
            measure(binary, args, tmp_path / 'out')


def test_checkpoint_still_requires_warmup_and_contiguous_frames(tmp_path):
    binary = tmp_path / 'host'
    binary.write_text('''#!/usr/bin/env python3
import os
from pathlib import Path
Path(os.environ['FF8_FRAME_TIMINGS']).write_text(
    'vblank,wall_ms,cpu_ms,cycles,interpreted,compiled,idle,gte,cd_bytes\\n'
    '5,2,1,10,5,0,0,0,0\\n6,2,1,10,5,0,0,0,0\\n')
print('Stop: VBlank limit reached\\nHardware: cycles=60 VBlanks=6 ')
''')
    binary.chmod(0o755)
    card = tmp_path / 'card'; card.write_bytes(bytes(128))
    script = tmp_path / 'script'; script.write_text('1 -\n')
    checkpoint = tmp_path / 'state'; checkpoint.write_bytes(b'synthetic checkpoint')
    args = SimpleNamespace(card=card, exe=binary, script=script, renderer='soft',
                           last=6, first=6, disc=[], interpreter=False)
    assert measure(binary, args, tmp_path / 'good', checkpoint)['frames'] == 1
    args.first = 5
    with pytest.raises(ValueError, match='missing or repeated'):
        measure(binary, args, tmp_path / 'no-warmup', checkpoint)
