"""Native PMON metadata stays current without replaying a register-read backlog."""
from pathlib import Path
import subprocess


def test_live_pitch_preserves_guest_reads_dma_and_irq(tmp_path):
    root=Path(__file__).resolve().parents[1]
    binary=tmp_path/'pitch'
    subprocess.run(['cc','-std=c99','-O2','-Wall','-Wextra','-Werror','-DFF8_PLAY',
                    str(root/'tests/spu_play_pitch_harness.c'),'-o',str(binary)],check=True)
    subprocess.run([str(binary)],check=True,timeout=90)
