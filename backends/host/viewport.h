#ifndef FF8_HOST_VIEWPORT_H
#define FF8_HOST_VIEWPORT_H
#include "options.h"
#include "gpu.h"
#ifdef __cplusplus
extern "C" {
#endif
typedef struct { int x, y, w, h; } host_rect_t;
typedef struct { host_rect_t frame, content; } host_viewport_t;
/* frame is the TV aperture/clip, content is the positioned scanout texture.
 * Width/height are gpu_display's hardware-rounded source pixel dimensions. */
host_viewport_t host_viewport(gpu_scanout_t, int width, int height, int output_w, int output_h, int mode);
#ifdef __cplusplus
}
#endif
#endif
