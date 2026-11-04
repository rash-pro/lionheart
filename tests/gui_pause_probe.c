/* Relink the real player objects with read-only observations of its modal loop. */
#include "cpu.h"
#include "gui.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static cpu_t *guest;
static cpu_t snapshot;
static unsigned opens, frames;
static int paused, failed;

void __real_cpu_init(cpu_t *);
void __real_host_gui_open(void);
void __real_host_gui_frame(unsigned, unsigned);

static void frozen(void) {
    if (!guest || memcmp(guest, &snapshot, sizeof(snapshot))) {
        fputs("GUI pause proof: guest changed inside modal loop\n", stderr);
        failed = 1;
        exit(99);
    }
}
static void summary(void) {
    fprintf(stderr, "GUI pause proof: opens=%u frames=%u frozen=%d\n", opens, frames, !failed);
}
void __wrap_cpu_init(cpu_t *cpu) {
    guest = cpu;
    __real_cpu_init(cpu);
}
void __wrap_host_gui_open(void) {
    if (!opens) atexit(summary);
    if (!guest || paused) { failed = 1; exit(98); }
    memcpy(&snapshot, guest, sizeof(snapshot));
    paused = 1;
    ++opens;
    __real_host_gui_open();
    frozen();
    paused = 0;
}
void __wrap_host_gui_frame(unsigned width, unsigned height) {
    if (paused) {
        /* Simulate one retired instruction to prove the observer fails closed. */
        if (!frames && getenv("FF8_GUI_PAUSE_PROBE_STEP")) guest->cycles += CYCLES_PER_INSN;
        frozen();
        ++frames;
    }
    __real_host_gui_frame(width, height);
    if (paused) frozen();
}
