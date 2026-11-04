"""Experimental policies retain CPU semantics away from device deadlines."""
from types import SimpleNamespace
from unittest.mock import patch

import pytest

from recomp.discover import Image, discover
from recomp.emit import emit
from recomp.timing import TimingEmitter
from recomp.__main__ import main_functions
import test_recompiler as reference


def test_exact_is_default(tmp_path):
    image = Image(bytes.fromhex('010008240800e00300000000'), 0x80010000, 0x80010000)
    for name, kwargs in [('default', {}), ('exact', {'timing': 'exact'})]:
        emit(image, discover(image), tmp_path / name, **kwargs)
    default = {p.name: p.read_bytes() for p in (tmp_path / 'default').iterdir()}
    explicit = {p.name: p.read_bytes() for p in (tmp_path / 'exact').iterdir()}
    assert default == explicit
    assert b'timing.h' not in default['generated.h']


def test_invalid_policy(tmp_path):
    image = Image(bytes(8), 0x80010000, 0x80010000)
    with pytest.raises(ValueError, match='timing policy'):
        emit(image, {}, tmp_path, timing='invalid')
    with pytest.raises(ValueError, match='requires fast'):
        emit(image, {}, tmp_path, timing='block', mode='trace')


def test_timing_entries_do_not_change_exact_discovery(tmp_path):
    import struct
    words = [reference.r(8, rs=31), 0, 0, 0, reference.imm(9, 0, 2, 42), reference.r(8, rs=31), 0]
    image = Image(struct.pack('<7I', *words), reference.BASE, reference.BASE,
                  timing_entry_points=(reference.BASE + 16,))
    exact = main_functions(image, 'exact')
    assert reference.BASE + 16 not in exact
    for mode in ['block', 'loose']:
        functions = main_functions(image, mode)
        assert reference.BASE + 16 in functions
        result = emit(image, functions, tmp_path / mode, timing=mode)
        assert result['timing_entry_points'] == [hex(reference.BASE + 16)]


@pytest.fixture(scope='module', params=['block', 'loose', 'block-inline', 'loose-inline', 'block-cache', 'loose-cache'])
def relaxed(request):
    mode=request.param.split('-')[0]
    def policy(*args, **kwargs):
        from pathlib import Path
        if request.param.endswith('-cache'):
            kwargs['register_cache']=dict(max_registers=8,functions=[f'f_{pc:08x}' for pc in args[1]])
        result=emit(*args, **kwargs, timing=mode)
        if '-' in request.param:
            header=Path(args[2])/'generated.h'
            header.write_text('#define FF8_TIMING_INLINE 1\n' +
                              ('#define FF8_PLAY_REG_CACHE 1\n' if request.param.endswith('-cache') else '') + header.read_text())
        return result
    extra = {f'timing_self_modify_{name}_{offset}': [0, 0, reference.imm(op, 20, 8, offset),
             reference.imm(9, 0, 9, 1)] + reference.RET
             for name, op in [('swl', 42), ('swr', 46)] for offset in range(4)}
    pcs = {name: reference.BASE + (len(reference.CASES) + n) * 0x200
           for n, name in enumerate(extra)}
    with patch.dict(reference.CASES, extra), patch.dict(reference.PCS, pcs), patch.object(reference, 'emit', policy):
        yield reference.compiled.__wrapped__(SimpleNamespace(param='compact'))


@pytest.mark.parametrize('name', ['load_delay', 'load_overwrite', 'load_twice', 'load_zero',
                                'branch_delay', 'load_branch', 'load_in_slot', 'return_load',
                                'load_slot_taken', 'load_slot_fallthrough', 'load_slot_twice',
                                'jr_capture', 'memory', 'gte', 'call_direct', 'call_indirect',
                                'call_return_load', 'incoming_load_twice', 'call_alt_link',
                                'tail_jump', 'fallback_entry', 'jump_table', 'unaligned_0', 'unaligned_1',
                                'unaligned_2', 'unaligned_3', 'swl_0', 'swl_1', 'swl_2', 'swl_3',
                                'swr_0', 'swr_1', 'swr_2', 'swr_3'])
def test_cpu_semantics(relaxed, name):
    reference.diff(relaxed, name)


@pytest.mark.parametrize('base', [0x80100000, 0xa0100000, 0x1f800000, 0x9f800000, 0xbf800000])
def test_memory_aliases_and_scratch(relaxed, base):
    reference.diff(relaxed, 'memory', {20: base}, {base: 0x44332211, base + 4: 0x88776655})


def test_watched_store_invalidates(relaxed):
    state, *_ = reference.diff(relaxed, 'self_modify',
                              {20: reference.PCS['self_modify'] + 4,
                               8: reference.imm(9, 0, 9, 77)})
    assert state[9] == 77


@pytest.mark.parametrize('store', ['swl', 'swr'])
@pytest.mark.parametrize('offset', range(4))
def test_merged_store_invalidates_before_the_next_instruction(relaxed, store, offset):
    name = f'timing_self_modify_{store}_{offset}'
    replacement = reference.imm(9, 0, 9, 77) if store == 'swl' else reference.imm(15, 0, 9, 1)
    value = (replacement << ((3 - offset) * 8)) & 0xffffffff if store == 'swl' else replacement >> (offset * 8)
    state, *_ = reference.diff(relaxed, name,
                              {20: reference.PCS[name] + 12, 8: value})
    assert state[9] == (77 if store == 'swl' else 65536)


def test_wait_hint_is_experimental():
    import struct
    words = [reference.imm(35, 20, 8, 0), 0, reference.imm(5, 8, 0, -3), 0]
    image = Image(struct.pack('<4I', *words), reference.BASE, reference.BASE)
    functions = discover(image)
    function = functions[reference.BASE]
    assert TimingEmitter(function, functions, loose=True).wait_loop(reference.BASE)
    assert 'c_timing_idle(c)' in TimingEmitter(function, functions, loose=True).emit()
    assert 'c_timing_idle(c)' not in TimingEmitter(function, functions).emit()
