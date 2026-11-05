#ifndef FF8_GPU_H
#define FF8_GPU_H
#include <stdint.h>
#include "frame_tags.h"

typedef struct gpu gpu_t;
gpu_t *gpu_create(void);
void gpu_gp0(gpu_t *, uint32_t word);
/* Contiguous little-endian RAM words, preserving partial command state. */
void gpu_gp0_words(gpu_t *, const uint8_t *, unsigned words);
#if defined(FF8_FRAME_PROVENANCE)
void gpu_frame_tags_bind(gpu_t *, struct cpu *);
int gpu_frame_tags_triangle(gpu_t *, frame_corner_t [3]);
void gpu_gp0_tagged(gpu_t *, uint32_t, frame_tag_t);
void gpu_gp0_ram(gpu_t *, struct cpu *, uint32_t, unsigned);
#endif
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

#endif
