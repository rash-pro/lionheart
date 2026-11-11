/* Smooth motion (--smooth-motion): the frames between the game's own, drawn
 * while it runs (docs/frame-present.md). Off, nothing here runs: no record,
 * no observer, the plain GTE entries bound. */
#ifndef HOST_FRAME_PRESENT_HOST_H
#define HOST_FRAME_PRESENT_HOST_H
#include "cpu.h"
#include "gpu.h"

/* Starts the frame identity service with the recording GTE entries bound, taps
 * the guest's GPU and creates the presentation GPU. fractions: the renderer
 * draws above the PlayStation's resolution. precision: PGXP's mode (1 vertex,
 * 2 perspective), which the presentation GPU keeps where it draws fractions.
 * Returns 0 on failure, with nothing left running. */
int host_frame_present_start(cpu_t *c, int fractions, int precision);
int host_frame_present_running(void);
/* The presentation GPU, or NULL when smooth motion is off: the GPU a renderer
 * mirrors and the display shows. */
gpu_t *host_frame_present_gpu(void);
/* Draws the presentation GPU for the moment since VBlanks after the guest's
 * last VBlank (0 at it, see frame_present.h); whole: as the game shows it now
 * (fast-forward). Returns the presentation GPU. */
gpu_t *host_frame_present_draw(cpu_t *c, double since, int whole);
/* Another thread draws (on): the guest's traffic is queued for it, and it takes
 * it up when it draws or calls host_frame_present_take; rebind starts its
 * renderer over when it takes up a restored state. */
void host_frame_present_queue(int on, void (*rebind)(gpu_t *));
void host_frame_present_take(void);
/* A state was restored, or the guest's GPU otherwise changed under the
 * presenter: start over from it. */
void host_frame_present_restored(cpu_t *c);
/* Unbinds and frees; prints a summary on stderr when report is set. */
void host_frame_present_stop(int report);
#endif
