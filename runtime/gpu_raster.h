#ifndef FF8_GPU_RASTER_H
#define FF8_GPU_RASTER_H
#include <stdint.h>

/* Decoded primitives and VRAM operations: the replacement boundary for a presentation backend. */
typedef struct {
    int x, y, r, g, b, u, v;
} gpu_vertex_t;

typedef struct {
    int textured, raw, semi, dither;
    unsigned clut;
} gpu_paint_t;

typedef struct {
    uint16_t vram[1024 * 512];
    unsigned mode, window, mask;
    int left, top, right, bottom;
    int display_x, display_y, display_w, display_h, display_enabled;
    int interlaced, field;
    /* Presentation only, never saved (gpu_subpixel): what to add, in pixels, to
     * the vertices of the primitive being drawn. shift applies to everything;
     * corner to the triangle being drawn when cornered is set. corner_w is
     * each corner's depth for perspective texturing, 0 for none. */
    float shift_x, shift_y;
    float corner[3][2];
    float corner_w[3];
    int cornered;
    /* Optional presentation diagnostic: every accepted fragment, even unchanged. */
    void (*fragment)(void *, int x, int y, int semi);
    void *fragment_context;
} gpu_raster_t;

void gpu_raster_triangle(gpu_raster_t *, gpu_vertex_t a, gpu_vertex_t b,
                         gpu_vertex_t c, gpu_paint_t paint);
void gpu_raster_rectangle(gpu_raster_t *, gpu_vertex_t origin, int w, int h, gpu_paint_t paint);
void gpu_raster_line(gpu_raster_t *, gpu_vertex_t a, gpu_vertex_t b, gpu_paint_t paint, int skip_first);
void gpu_raster_fill(gpu_raster_t *, int x, int y, int w, int h, uint32_t color);
void gpu_raster_copy(gpu_raster_t *, int sx, int sy, int dx, int dy, int w, int h);
void gpu_raster_store(gpu_raster_t *, int x, int y, uint16_t pixel);

/* Little-endian pixels on one upload row; x may wrap around VRAM. */
void gpu_raster_store_span(gpu_raster_t *, unsigned x, unsigned y, const uint8_t *, unsigned pixels);

struct gpu;
gpu_raster_t *gpu_raster_state(struct gpu *);

#endif
