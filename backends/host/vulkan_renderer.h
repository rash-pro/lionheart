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
/* Smooth motion's clock: acquires the window's next image when the swapchain
 * has one free (FIFO frees one each refresh): 1 held, 0 not yet, -1 the window
 * cannot be presented to now. The next host_gpu_present draws into it. */
int host_gpu_ready(void);
/* When pictures reach the display, where the device can say (VK_KHR_present_wait):
 * the id of the latest present to the window (0 when it cannot say), and each
 * present seen reaching the display since the last call, with when (SDL's
 * performance counter, in seconds); 0 when none is waiting. */
uint64_t host_gpu_present_id(void);
int host_gpu_present_seen(uint64_t *id, double *seconds);
/* Present timing (VK_EXT_present_timing) on the window: 0 none, 1 when each
 * picture was shown, 2 also presents held until a time (host_gpu_present_at).
 * With it, host_gpu_present_seen gives the times the presentation engine
 * reports, and host_gpu_refresh the display's refresh period in seconds as it
 * knows it (0 when it does not). */
int host_gpu_present_timing(void);
double host_gpu_refresh(void);
/* The next present is not shown before this time (SDL's counter, seconds). */
void host_gpu_present_at(double seconds);
/* The host paces its presents itself (smooth motion): where presents can be
 * held until a time, the window's swapchain is a mailbox, each picture held
 * by the compositor until its refresh; elsewhere it shows the latest picture
 * ready at each refresh where the device can, and is FIFO otherwise. */
void host_gpu_paced(int on);
int host_gpu_present(void);
int host_gpu_scanout(gpu_t *);
int host_gpu_rgb(gpu_t *, const uint8_t **rgb, int *w, int *h);
/* Explicit replay verification only; guest reads never call this API. */
int host_gpu_download(gpu_t *, uint16_t *out, int raw_parallel);
int host_gpu_resume(gpu_t *);
/* Smooth motion: keep VRAM as it stands as the point drawing starts again
 * from (restore 0), or put it back (restore 1); only what was written since
 * is copied. checkpoints turns this on for the GPU the renderer mirrors. */
void host_gpu_checkpoints(int on);
void host_gpu_checkpoint(gpu_t *, int restore);
/* Smooth motion draws a picture between game frames (on) or a whole one: above
 * the PlayStation's resolution, the software rasterizer only draws, in the
 * pictures between, where the renderer reads its pixels (copies it draws
 * natively, feedback textures) as the picture before did, taking VRAM back
 * from the GPU first where it reads elsewhere. */
void host_gpu_between(int on);
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
