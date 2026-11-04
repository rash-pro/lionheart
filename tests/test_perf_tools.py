"""Receipts bind a scene to independent save-card inputs and exact boundaries."""
import importlib.util
from pathlib import Path
from types import SimpleNamespace

import pytest

from tools.perf_report import summarize


def test_scene_counter_deltas_require_exact_boundaries():
    log = '\n'.join([
        '[ 0.1] CPU Extensions Detected:',
        '[ 0.123] CPU MEMORY vb=30 ram=10 scratch=1',
        'CPU DISPATCH vb=30 call=4 candidate_lost=0',
        'CPU MEMORY vb=60 ram=40 scratch=21',
        'CPU DISPATCH vb=60 call=14 candidate_lost=0'])
    result = summarize(log, 30, 60)
    assert result['counters'] == {'memory.ram': 30, 'memory.scratch': 20,
                                   'dispatch.call': 10, 'dispatch.candidate_lost': 0}
    with pytest.raises(ValueError, match='missing exact'):
        summarize(log, 31, 60)
