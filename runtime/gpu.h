#ifndef FF8_GPU_H
#define FF8_GPU_H
#include <stdint.h>

typedef struct gpu gpu_t;
gpu_t *gpu_create(void);
void gpu_gp0(gpu_t *, uint32_t word);
/* Contiguous little-endian RAM words, preserving partial command state. */
void gpu_gp0_words(gpu_t *, const uint8_t *, unsigned words);
void gpu_gp1(gpu_t *, uint32_t word);
uint32_t gpu_read(gpu_t *);
uint32_t gpu_status(gpu_t *);
uint32_t gpu_status_scanline(gpu_t *, unsigned line);
void gpu_vblank(gpu_t *);
const uint16_t *gpu_vram(gpu_t *);
void gpu_display(gpu_t *, int *x, int *y, int *w, int *h, int *rgb24, int *enabled);
/* Host presentation reads GP1 timing without changing the guest read latch. */
typedef struct { uint32_t horizontal, vertical, mode; } gpu_scanout_t;
gpu_scanout_t gpu_scanout(const gpu_t *);

/* A read-only view of what the guest asks the GPU to show: every GP1(05h)
 * display-start write, every VBlank, and every polygon packet as it is drawn
 * (opcode, then the packet's words). Without an observer, the default and the
 * state of a new GPU, each of these costs one test. Observers must not call
 * back into the GPU. */
typedef struct {
    void (*display_start)(void *context, uint32_t start);
    void (*vblank)(void *context);
    void (*polygon)(void *context, unsigned opcode, const uint32_t *packet);
} gpu_observer_t;
void gpu_observe(gpu_t *, const gpu_observer_t *, void *context);

/* Everything the guest sends the GPU, in order, for a recording that replays
 * through another GPU: GP0 words (little-endian, as in RAM), GP1 words before
 * they take effect, each VRAM read and each VBlank before the field changes.
 * A tap may read the GPU (gpu_display, gpu_vram) but not drive it. Without
 * one, the default and the state of a new GPU, each costs one test. */
typedef struct {
    void (*gp0)(void *context, const uint8_t *words, unsigned count);
    void (*gp1)(void *context, uint32_t word);
    void (*read)(void *context);
    void (*vblank)(void *context);
} gpu_tap_t;
void gpu_tap(gpu_t *, const gpu_tap_t *, void *context);

#endif
