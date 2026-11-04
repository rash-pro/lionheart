"""Overlay extraction facts and executable loader-lifetime regressions."""
from pathlib import Path
import csv
import importlib.util
import os
import struct
import subprocess

import pytest

from recomp.overlays import infer_effect_base


def test_effect_address_inference_rejects_assets_and_ambiguity():
    with pytest.raises(ValueError, match="ambiguous"):
        infer_effect_base(bytes(512))
    # j 0x801a0010; nop; data; data; addiu sp,sp,-16; jr ra; nop
    data = struct.pack("<7I", 0x08068004, 0, 0xFFFFFFFF, 0, 0x27BDFFF0, 0x03E00008, 0)
    base, evidence = infer_effect_base(data)
    assert base == 0x801A0000
    assert evidence["examples"][0]["target_offset"] == "0x10"


@pytest.fixture(scope="module")
def overlay_probe(tmp_path_factory):
    output = tmp_path_factory.mktemp("overlay-lifetime") / "probe"
    command = ["cc", "-std=gnu99", "-O2", "-DFF8_STATS", "-Iruntime",
               "tests/overlay_dispatch_harness.c", *map(str, Path("runtime").glob("*.c")), "-o", str(output)]
    subprocess.run(command, check=True, capture_output=True)
    return output


@pytest.mark.parametrize("mode", range(7), ids=["cached-miss", "nested-return", "callback-unwind", "cpu-stop",
                                               "return-from-exception", "outer-unwind", "interpreter-only"])
def test_dispatch_pins_native_modules(overlay_probe, mode):
    subprocess.run([str(overlay_probe), str(mode)], check=True, timeout=10)


def test_overlay_profile_counts_uncompiled_catalog_sets(overlay_probe, tmp_path):
    catalog = tmp_path / "catalog.txt"
    output = tmp_path / "counts.csv"
    # Mode 6 executes two interpreted guest words without invoking a native hook.
    catalog.write_text("511 80098000 8 1 80098000 03e00008\n")
    # The dispatch harness returns normally, so use an instruction-budget stop
    # in a separate tiny executable to exercise the normal report flush as well.
    source = tmp_path / "profile.c"
    source.write_text('''
#include "hw.h"
#include <stdlib.h>
int main(void) {
    cpu_t *c = malloc(sizeof(*c));
    cpu_init(c); hw_get(c);
    mem_write32(c, 0x80098000, 0x03e00008); mem_write32(c, 0x80098004, 0);
    c->interpreter_only = 1; dispatch(c, 0x80098000);
    c_count_recompiled(c, 0x80098000, 3);
    cpu_stop(c, "profile done"); hw_destroy(c); free(c); return 0;
}
''')
    binary = tmp_path / "profile"
    subprocess.run(["cc", "-std=gnu99", "-O1", "-DFF8_STATS", "-Iruntime", str(source),
                    *map(str, Path("runtime").glob("*.c")), "-o", str(binary)], check=True, capture_output=True)
    subprocess.run([str(binary)], check=True, env=dict(os.environ, FF8_OVERLAY_CATALOG=str(catalog), FF8_OVERLAY_PROFILE=str(output)))
    assert list(csv.DictReader(output.open())) == [{"index": "511", "interpreter": "2", "recompiled": "3"}]
