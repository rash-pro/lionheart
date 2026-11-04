"""Pure getter polling, including code in another page and IRQ-owned flags."""
import ctypes
from pathlib import Path
import struct
import subprocess

import pytest

from recomp.discover import Image, discover
from recomp.emit import emit
from test_recompiler import imm, r, j


@pytest.fixture(params=[('compact','ram'), ('helper','ram'), ('fast','ram'),
                        ('compact','scratch'), ('helper','cross'),
                        ('compact','side_effect'), ('compact','mmio'), ('nostats','ram')])
def polling(tmp_path, request):
    mode, shape = request.param
    base = 0x80010ff4 if shape == 'cross' else 0x80010000
    head, getter = base + 4, base + (0x200c if shape == 'cross' else 0x100)
    address = 0x1f800000 if shape == 'scratch' else 0x1f801070 if shape == 'mmio' else 0x80100004
    low = address & 0xffff
    high = ((address + 0x8000) >> 16) & 0xffff
    words = [r(33, 31, 0, 16), j(3, getter), 0, imm(5, 2, 0, -3), 0, r(8, rs=16), 0]
    words += [0] * ((getter - base)//4 - len(words))
    words += [imm(15, 0, 2, high), imm(35, 2, 2, low), r(8, rs=31),
              imm(43, 20, 9, 0) if shape == 'side_effect' else 0]
    data = struct.pack(f'<{len(words)}I', *words)
    image = Image(data, base, base)
    emit(image, discover(image), tmp_path)
    (tmp_path/'probe.c').write_text('''
#include "cpu.h"
#include "generated.h"
#include <stdlib.h>
#define FLAG FLAG_ADDRESS
static uint32_t flag(cpu_t *c) { return FLAG == 0x1f801070 ? c->mmio[0] : mem_read32(c, FLAG); }
void bios_call(cpu_t *c, unsigned t) { (void)c; (void)t; }
void bios_syscall(cpu_t *c) { (void)c; }
void gte_cmd(cpu_t *c, uint32_t v) { (void)c; (void)v; }
uint32_t gte_read(cpu_t *c, unsigned r, int t) { (void)c; (void)r; (void)t; return 0; }
void gte_write(cpu_t *c, unsigned r, uint32_t v, int t) { (void)c; (void)r; (void)v; (void)t; }
uint32_t hw_read(cpu_t *c, uint32_t a, unsigned w) { (void)a; (void)w; ++c->mmio_epoch; return c->mmio[0]; }
void hw_write(cpu_t *c, uint32_t a, unsigned w, uint32_t v) { (void)c; (void)a; (void)w; (void)v; }
void hw_event(cpu_t *c) {
    if (FLAG == 0x1f801070) c->mmio[0] = 0;
    else if (FLAG < 0x1f800000u || FLAG >= 0x1f800400u) ram_write(c, FLAG, 4, 0);
    else mem_write32(c, FLAG, 0);
    c->next_event = UINT64_MAX/2;
}
uint64_t check(const void *data, unsigned interpret, unsigned deadline, unsigned control, uint64_t *out) {
    cpu_t *c = malloc(sizeof(*c)); uint64_t accelerated;
    cpu_init(c); memcpy(c->ram + (BASE & (RAM_SIZE-1)), data, DATA_SIZE);
    c->entries = generated_entries; c->entry_count = generated_count; c->entry_regions = generated_regions;
    if (NATIVE_ABSENT) while (c->entry_count && c->entries[c->entry_count-1].pc >= GETTER) --c->entry_count;
    c->r[2] = 0x12345678; c->r[9] = 0x11223344; c->r[20] = 0x80100008;
    if (FLAG == 0x1f801070) c->mmio[0] = 1; else mem_write32(c, FLAG, 1);
    if (control == 2) c->ram_top = FLAG == 0x1f800000 ? 0 : 0x100000;
    c_set_sr(c, control == 1 ? 0x10401 : 0x401);
    if (control == 3) mem_write32(c, GETTER+12, 0xae890000); /* impure, runtime-replaced getter */
    c->interpreter_only = interpret; c->next_event = deadline;
    c->instruction_limit = control == 4 ? 101 : 0; c->stall_limit = 0;
    dispatch(c, BASE);
    for (unsigned i = 0; i < 32; ++i) out[i] = c->r[i];
    out[32] = c->pc; out[33] = c->cycles; out[34] = c->load_reg; out[35] = c->load_value;
    out[36] = flag(c); out[37] = mem_read32(c, 0x80100008); out[38] = c_instructions(c); out[39] = c->halted;
    for (unsigned i = 0; i < 32; ++i) out[40+i] = c->cop0[i];
    out[72] = c->branch_pending; out[73] = c->branch_pending ? c->branch_target : 0; out[74] = c->mmio_epoch;
    #ifndef FF8_STATS
    if (!c->instruction_limit) out[38] = 0;
#endif
    accelerated = c->idle_instructions; free(c); return accelerated;
}
'''.replace('FLAG_ADDRESS', hex(address)+'u').replace('DATA_SIZE', str(len(data)))
       .replace('BASE', hex(base)+'u').replace('GETTER', hex(getter)+'u')
       .replace('NATIVE_ABSENT','1' if shape == 'cross' else '0'))
    flags = {'compact':['-DFF8_COMPACT'], 'helper':['-DFF8_COMPACT=1'], 'nostats':['-DFF8_COMPACT']}.get(mode, [])
    binary = tmp_path/'probe.so'
    subprocess.run(['cc','-std=c99','-O2',*(['-DFF8_STATS'] if mode != 'nostats' else []),*flags,'-shared','-fPIC',
                    '-Iruntime','-I'+str(tmp_path),'runtime/cpu.c','runtime/interp.c',
                    *map(str,tmp_path.glob('*.c')),'-o',str(binary)],check=True,capture_output=True)
    lib = ctypes.CDLL(str(binary))
    lib.check.argtypes = [ctypes.c_void_p,ctypes.c_uint,ctypes.c_uint,ctypes.c_uint,ctypes.POINTER(ctypes.c_uint64)]
    lib.check.restype = ctypes.c_uint64
    return lib, data, shape, mode


def test_poll_matches_every_event_boundary(polling):
    lib, data, shape, mode = polling
    accelerated = 0
    for deadline in range(1, 401):
        a,b = (ctypes.c_uint64*75)(),(ctypes.c_uint64*75)()
        accelerated += lib.check(data,0,deadline,0,a)
        lib.check(data,1,deadline,0,b)
        assert list(a) == list(b), (shape,deadline,list(a),list(b))
    assert (accelerated > 0) == (shape not in ('side_effect','mmio') and mode != 'nostats')


def test_poll_rejects_isolation_fences_and_replaced_getters(polling):
    lib, data, shape, mode = polling
    for control in (1,2,3,4):
        for deadline in (31,69,129,299,401):
            a,b = (ctypes.c_uint64*75)(),(ctypes.c_uint64*75)()
            accelerated = lib.check(data,0,deadline,control,a)
            lib.check(data,1,deadline,control,b)
            assert list(a) == list(b), (shape,control,deadline)
            if control != 4: assert accelerated == 0


@pytest.mark.parametrize('direct',[False,True])
def test_poll_flag_cleared_by_real_irq_handler(tmp_path,direct):
    base,getter,handler = 0x80010000,0x80010100,0x80010200
    words=[r(33,31,0,16),j(3,getter),0,imm(5,2,0,-3),0,r(8,rs=16),0]
    words += [0]*(64-len(words))
    words += [imm(15,0,2,0x8010),imm(35,2,2,4),r(8,rs=31),0]
    words += [0]*(128-len(words))
    words += [imm(15,0,8,0x8010),imm(43,8,0,4),r(8,rs=31),0]
    data=struct.pack(f'<{len(words)}I',*words)
    image=Image(data,base,base,entry_points=(handler,))
    emit(image,discover(image),tmp_path)
    (tmp_path/'probe.c').write_text('''
#include "hw.h"
#include "generated.h"
#include <stdlib.h>
void check(const void *data,unsigned interpret,unsigned target,uint64_t *out) {
    cpu_t *c=malloc(sizeof(*c)); cpu_init(c); memcpy(c->ram+0x10000,data,DATA_SIZE);
    c->instruction_limit=c->stall_limit=0; hw_get(c);
    c->entries=generated_entries; c->entry_count=generated_count; c->entry_regions=generated_regions;
    mem_write32(c,0x80100004,1);
    c->hw->events[0]=(bios_event_t){0xf2000002u,2,0x1000,0x80010200,2};
    hw_write(c,0x1f801114,2,0x100);
    hw_write(c,0x1f801128,2,target); hw_write(c,0x1f801124,2,0x58);
    hw_write(c,0x1f801074,2,0x40); c_set_sr(c,0x401);
    c->interpreter_only=interpret; dispatch(c,0x80010000);
    for(unsigned i=0;i<32;++i) out[i]=c->r[i];
    out[32]=c->pc;out[33]=c->cycles;out[34]=c->load_reg;out[35]=c->load_value;
    out[36]=mem_read32(c,0x80100004);out[37]=c_instructions(c);out[38]=c->halted;
    out[39]=c->hw->irq_delivered[6];out[40]=c->hw->event_calls[HW_VIDEO];
    out[41]=c->interp_instructions;out[42]=c->idle_instructions;
    hw_destroy(c);free(c);
}
'''.replace('DATA_SIZE',str(len(data))))
    binary=tmp_path/'probe.so'
    subprocess.run(['cc','-std=c99','-O2','-DFF8_STATS','-shared','-fPIC',
                    '-DFF8_COMPACT' if direct else '-DFF8_COMPACT=1','-Iruntime','-I'+str(tmp_path),
                    *map(str,sorted(Path('runtime').glob('*.c'))),*map(str,tmp_path.glob('*.c')),
                    '-o',str(binary)],check=True,capture_output=True)
    lib=ctypes.CDLL(str(binary))
    lib.check.argtypes=[ctypes.c_void_p,ctypes.c_uint,ctypes.c_uint,ctypes.POINTER(ctypes.c_uint64)]
    for target in range(9990,10020):
        a,b=(ctypes.c_uint64*43)(),(ctypes.c_uint64*43)()
        lib.check(data,0,target,a);lib.check(data,1,target,b)
        assert list(a)[:41] == list(b)[:41],(direct,target,list(a),list(b))
        assert a[36]==0 and a[39]==1 and a[40]>=8
        assert a[41]<64 and a[42]>4000,(direct,target,list(a))
