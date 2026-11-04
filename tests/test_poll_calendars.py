"""Polling must observe SPU IRQs and RAM materialized by deferred MDEC DMA."""
import ctypes as C
from pathlib import Path
import struct
import subprocess

import pytest

from recomp.discover import Image, discover
from recomp.emit import emit
from test_recompiler import imm, j, r


@pytest.fixture(params=[False, True], scope='module')
def calendar_poll(tmp_path_factory, request):
    output = tmp_path_factory.mktemp('poll-calendars')
    base, getter, handler = 0x80010000, 0x80010100, 0x80010200
    words = [r(33, 31, 0, 16), j(3, getter), 0, imm(5, 2, 0, -3), 0, r(8, rs=16), 0]
    words += [0] * (64 - len(words))
    words += [imm(15, 0, 2, 0x8018), imm(35, 2, 2, 0), r(8, rs=31), 0]
    words += [0] * (128 - len(words))
    words += [imm(15, 0, 8, 0x8018), imm(43, 8, 0, 0), r(8, rs=31), 0]
    data = struct.pack(f'<{len(words)}I', *words)
    image = Image(data, base, base, entry_points=(handler,))
    emit(image, discover(image), output)
    (output / 'probe.c').write_text('''
#include "hw.h"
#include "generated.h"
#include <assert.h>
#include <stdlib.h>
void check(const void *data, unsigned interpret, unsigned control, unsigned media,
           unsigned phase, uint64_t *out) {
    static const spu_backend_t sink = {NULL, NULL};
    cpu_t *c = malloc(sizeof(*c));
    assert(c); cpu_init(c); hw_get(c);
    memcpy(c->ram + 0x10000, data, DATA_SIZE);
    c->entries = generated_entries; c->entry_count = generated_count;
    c->entry_regions = generated_regions;
    c->instruction_limit = 100000; c->stall_limit = 0;
    c->cycles = phase; hw_sync(c);
    if (control) spu_set_backend(c->hw->spu, &sink, NULL);
    mem_write32(c, 0x80180000, 1);
    if (media == 1) {
        hw_write(c, 0x1f801824, 4, 0xe0000000);
        hw_write(c, 0x1f801820, 4, 0x24000004);
        for (unsigned i = 0; i < 4; ++i) hw_write(c, 0x1f801820, 4, 0xfe000000);
        hw_write(c, 0x1f8010f0, 4, 0x88);
        hw_write(c, 0x1f801090, 4, 0x180000);
        hw_write(c, 0x1f801094, 4, 0x10020);
        hw_write(c, 0x1f801098, 4, 0x01000200);
        assert(c->ram_limit <= 0x180000 && c->hw->media_bound != HW_NEVER);
    } else if (media == 2) {
        hw_write(c, 0x1f801da6, 2, 0);
        hw_write(c, 0x1f8010f0, 4, 0x80000);
        hw_write(c, 0x1f8010c0, 4, 0x180000);
        hw_write(c, 0x1f8010c4, 4, 0x10001);
        hw_write(c, 0x1f8010c8, 4, 0x01000200);
    } else {
        c->hw->events[0] = (bios_event_t){0xf000000au, 0x1000, 0x1000, 0x80010200, 2};
        c->hw->auto_ack |= 0x200;
        hw_write(c, 0x1f801da4, 2, 0);
        hw_write(c, 0x1f801daa, 2, 0xc040);
        hw_write(c, 0x1f801074, 2, 0x200);
        c_set_sr(c, 0x401);
    }
    c->interpreter_only = interpret;
    dispatch(c, 0x80010000);
    hw_sync(c);
    for (unsigned i = 0; i < 32; ++i) out[i] = c->r[i];
    out[32] = c->pc; out[33] = c->cycles; out[34] = c->load_reg; out[35] = c->load_value;
    out[36] = mem_read32(c, 0x80180000); out[37] = c_instructions(c); out[38] = c->halted;
    out[39] = c->hw->irq_delivered[9]; out[40] = c->hw->irq_raised[9];
    out[41] = c->hw->dma[1].madr; out[42] = c->hw->dma[1].chcr;
    out[43] = c->hw->mdec.words_out; out[44] = spu_read(c->hw->spu, 0x1ae);
    for (unsigned i = 0; i < 32; ++i) out[45 + i] = c->cop0[i];
    out[77] = c->idle_instructions; out[78] = c->hw->dma[4].madr;
    hw_destroy(c); free(c);
}
'''.replace('DATA_SIZE', str(len(data))))
    binary = output / 'probe.so'
    subprocess.run(['cc', '-std=c99', '-O2',
                    '-DFF8_STATS', '-DFF8_COMPACT' if request.param else '-DFF8_COMPACT=1',
                    '-shared', '-fPIC', '-Iruntime', '-I' + str(output),
                    *map(str, sorted(Path('runtime').glob('*.c'))),
                    *map(str, output.glob('*.c')), '-o', str(binary)], check=True, capture_output=True)
    lib = C.CDLL(str(binary))
    lib.check.argtypes = [C.c_void_p, *[C.c_uint] * 4, C.POINTER(C.c_uint64)]
    lib.check.restype = None
    return lib, data


@pytest.mark.parametrize('control', [False, True])
@pytest.mark.parametrize('media', [0, 1, 2], ids=['spu-irq', 'mdec-dma', 'spu-dma'])
def test_poll_observes_real_device_calendars(calendar_poll, control, media):
    lib, data = calendar_poll
    skipped = 0
    for phase in range(32):
        native, reference = (C.c_uint64 * 79)(), (C.c_uint64 * 79)()
        lib.check(data, 0, control, media, phase, native)
        lib.check(data, 1, control, media, phase, reference)
        assert list(native)[:77] == list(reference)[:77], (control, media, phase)
        assert native[36] == 0 and native[37] < 100000
        if media == 1:
            assert native[43] > 0
        elif media == 2:
            assert native[78] == reference[78] == 0x180004
        else:
            assert native[39] == native[40] == 1
        skipped += native[77]
    if media != 1:
        assert skipped > 0
