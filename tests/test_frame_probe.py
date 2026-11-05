"""The optional Linux probe owns its sampler through CPU replacement and teardown."""
import csv
import os
import platform
from pathlib import Path
import subprocess

import pytest

pytestmark = pytest.mark.skipif(platform.system() != 'Linux' or platform.machine() != 'x86_64',
                                reason='Linux x86-64 diagnostic probe')


@pytest.mark.parametrize('subsystems', [False, True])
def test_frame_probe_survives_cpu_replacement(tmp_path, subsystems):
    source = tmp_path / 'host.c'
    source.write_text('''
#include "hw.h"
#include <stdlib.h>
void cpu_init(cpu_t *c) { memset(c,0,sizeof(*c)); c->hw=calloc(1,sizeof(*c->hw)); }
void hw_destroy(cpu_t *c) { free(c->hw); c->hw=NULL; }
void host_present(gpu_t *g) { (void)g; }
''')
    main = tmp_path / 'main.c'
    main.write_text('''
#include "hw.h"
#include <stdlib.h>
void host_present(gpu_t *g);
int main(void) {
    cpu_t *old=malloc(sizeof(*old)), *c=malloc(sizeof(*c));
    cpu_init(old); old->cycles=100; old->interp_instructions=50;
    host_present(NULL);
    cpu_init(c); hw_destroy(old); free(old);
    for (unsigned frame=1; frame<=3; ++frame) {
        c->hw->vblanks=frame; c->cycles+=10; c->interp_instructions+=5;
        host_present(NULL);
    }
    hw_destroy(c); free(c);
    volatile unsigned value=0;
    for (unsigned i=0;i<10000000;++i) value+=i; /* Exercise time after CPU teardown. */
    return value==1;
}
''')
    binary = tmp_path / 'probe'
    subprocess.run(['cc', '-std=c99', '-O2', '-ffunction-sections', '-Iruntime',
                    *(['-DSUBSYSTEMS'] if subsystems else []), 'tools/frame_probe.c', str(source), str(main),
                    '-Wl,--gc-sections,--wrap=cpu_init,--wrap=host_present,--wrap=hw_destroy',
                    '-o', str(binary)], check=True)
    csv_path = tmp_path / 'frames.csv'
    subprocess.run([str(binary)], env=dict(os.environ, FF8_FRAME_TIMINGS=str(csv_path)), check=True)
    rows = list(csv.DictReader(csv_path.open()))
    assert [int(row['vblank']) for row in rows] == [1, 2, 3]
    assert all(int(row['cycles']) == 10 and int(row['interpreted']) == 5 for row in rows)
    if subsystems:
        assert Path(str(csv_path)+'.samples').read_text().startswith('# cpu_init=')
