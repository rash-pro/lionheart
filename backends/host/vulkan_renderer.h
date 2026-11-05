#ifndef FF8_HOST_VULKAN_RENDERER_H
#define FF8_HOST_VULKAN_RENDERER_H
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
#include "gpu.h"
struct SDL_Window;
/* One GPU per host process. Options are fixed for its lifetime. */
int host_gpu_configure(const char *renderer, unsigned scale, int true_color, unsigned msaa,
                       const char *scaled_frames, const char *timings);
int host_gpu_enabled(void);
int host_gpu_start(gpu_t *, struct SDL_Window *);
void host_gpu_stop(void);
int host_gpu_frame(gpu_t *);
#ifdef FF8_GUI
/* A paused/shell presentation submits cached images without guest scanout/uploads. */
int host_gpu_gui_frame(void);
int host_gpu_gui_rgb(const uint8_t **rgb, int *w, int *h);
/* Consume one completed GPU GUI-pass timing, or -1 until a query is ready. */
double host_gpu_gui_timing(void);
const char *host_gpu_device_name(void);
#endif
int host_gpu_present(void);
int host_gpu_scanout(gpu_t *);
int host_gpu_rgb(gpu_t *, const uint8_t **rgb, int *w, int *h);
/* Explicit replay verification only; guest reads never call this API. */
int host_gpu_download(gpu_t *, uint16_t *out, int raw_parallel);
int host_gpu_resume(gpu_t *);
/* A mode change or disabling discards only host presentation history. */
void host_gpu_interpolation(gpu_t *, int enabled);
void host_gpu_presentation(int scale_mode, int linear);
const char *host_gpu_scaled_frames(void);
/* CRT presets (P): 0 off, 1 pvm, 2 tv, 3 arcade; changeable at runtime. */
void host_gpu_crt(int preset);
int host_gpu_crt_preset(void);
/* Diagnostics: present every frame into an offscreen WxH target as well, and
 * read the last presented image back as tightly packed RGB. */
void host_gpu_present_capture(const char *dir, unsigned width, unsigned height);
const char *host_gpu_presented_frames(void);
int host_gpu_presented_rgb(const uint8_t **rgb, int *w, int *h);
/* Present a supplied scaled scanout (RGB, native*scale) offscreen; test/preview only. */
int host_gpu_present_image(const uint8_t *rgb, int width, int height, gpu_scanout_t timing, int native_w, int native_h,
                           unsigned out_w, unsigned out_h, int preset, const uint8_t **out, int *w, int *h);
#ifdef FF8_VULKAN_TESTING
void host_gpu_test_fault(unsigned phase);
unsigned host_gpu_test_maps(void);
#endif
#ifdef __cplusplus
}
#endif
#endif
