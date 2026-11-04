"""GTE instructions retain the interpreter's cycle and hardware event boundaries."""
import ctypes as C
import os
from pathlib import Path
import struct
import subprocess

import pytest

from recomp.discover import Image, discover
from recomp.emit import emit
from test_recompiler import cop, imm, r

ROOT = Path(__file__).resolve().parents[1]


@pytest.fixture(scope="module", params=["compact", "compact-nostats", "fast", "precise"])
def timed_gte(tmp_path_factory, request):
    out = tmp_path_factory.mktemp("gte-timing")
    words = [cop(18, 6, 8, 31), cop(18, 4, 8, 30), cop(18, 0, 9, 31),
             r(33, 9, 0, 10), r(33, 9, 0, 11), imm(50, 20, 0, 0),
             imm(50, 20, 1, 4), 0x4A080001, 0x4A080030, 0x4A0FE412,
             imm(4, 0, 0, 1), 0x4A000006, cop(18, 4, 8, 15),
             cop(18, 0, 9, 15), cop(18, 2, 12, 31), imm(58, 20, 14, 8),
             r(33, 12, 0, 13), r(8, rs=31), 0]
    data = struct.pack(f"<{len(words)}I", *words)
    image = Image(data, 0x80010000, 0x80010000)
    emit(image, discover(image), out, host_mode="precise" if request.param == "precise" else "fast")
    (out / "probe.c").write_text('''#include "cpu.h"
#include "generated.h"
#include <stdlib.h>
static unsigned events;
void bios_call(cpu_t *c, unsigned t) { (void)c; (void)t; }
void bios_syscall(cpu_t *c) { (void)c; }
uint32_t hw_read(cpu_t *c, uint32_t a, unsigned w) { (void)c; (void)a; (void)w; return 0; }
void hw_write(cpu_t *c, uint32_t a, unsigned w, uint32_t v) { (void)c; (void)a; (void)w; (void)v; }
void hw_event(cpu_t *c) {
    ++events;
    gte_write(c, 7, 65535, 1);
    gte_write(c, 15, 0xfedf0123u, 0);
    c->next_event = UINT64_MAX / 2;
}
void check(const void *data, unsigned interpret, unsigned deadline, uint64_t *out) {
    cpu_t *c = malloc(sizeof(*c));
    cpu_init(c); memcpy(c->ram + 0x10000, data, DATA_BYTES);
    c->entries = generated_entries; c->entry_count = generated_count;
    c->entry_regions = generated_regions;
    c->r[8] = 0x8001fedcu; c->r[9] = 0x12345678; c->r[20] = 0x80100000;
    ram_write(c, 0x100000, 4, 0x00100020); ram_write(c, 0x100004, 4, 0x0040);
    c->load_reg = 8; c->load_value = 0x5555aaaa;
    c_set_sr(c, 0x401); c->next_event = deadline;
    c->instruction_limit = c->stall_limit = 0;
    c->interpreter_only = interpret; events = 0;
    dispatch(c, 0x80010000);
    for (unsigned i = 0; i < 32; ++i) {
        out[i] = c->r[i]; out[32+i] = c->gte_data[i]; out[64+i] = c->gte_ctrl[i];
    }
    out[96] = c->cycles; out[97] = c->gte_commands; out[98] = c->pc;
    out[99] = c->load_reg; out[100] = c->load_value; out[101] = c->halted;
    out[102] = events; out[103] = ram_read(c, 0x100008, 4);
    free(c);
}
'''.replace("DATA_BYTES", str(len(data))))
    binary = out / "probe.so"
    flags = ["-DFF8_COMPACT"] if request.param.startswith("compact") else []
    if request.param != "compact-nostats":
        flags += ["-DFF8_STATS"]
    subprocess.run([os.environ.get("CC", "cc"), "-std=c99", "-O2", "-shared", "-fPIC",
                    "-DFF8_DIRECT_RAM", *flags, "-I" + str(ROOT / "runtime"), "-I" + str(out),
                    *[str(ROOT / "runtime" / name) for name in ("cpu.c", "interp.c", "gte.c")],
                    *map(str, out.glob("*.c")), "-o", str(binary)], check=True, capture_output=True)
    lib = C.CDLL(str(binary))
    lib.check.argtypes = [C.c_void_p, C.c_uint, C.c_uint, C.POINTER(C.c_uint64)]
    return lib, data, len(words)


def test_commands_and_transfers_at_every_deadline(timed_gte):
    lib, data, instructions = timed_gte
    observed = set()
    for deadline in range(1, 2 * instructions + 5):
        native, reference = (C.c_uint64 * 104)(), (C.c_uint64 * 104)()
        lib.check(data, 0, deadline, native)
        lib.check(data, 1, deadline, reference)
        assert list(native) == list(reference), (deadline, list(native), list(reference))
        assert native[96] == 2 * instructions
        assert native[97] == 4
        observed.add(native[102])
    assert observed == {0, 1}
