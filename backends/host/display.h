#ifndef FF8_HOST_DISPLAY_H
#define FF8_HOST_DISPLAY_H
#include "gpu.h"
#include "sio.h"
#include <stddef.h>
#include <stdint.h>

/* One host window per process; hw invokes host_present once per VBlank. */
int host_display_init(int headless, const char *dump_frames, const char *dump_vram);
int host_display_configure(int fullscreen, int scale_mode, int linear);
void host_present(gpu_t *);
unsigned host_input_state(void);
void host_display_state_slot(unsigned);
void host_display_crt(int preset);
int host_display_crt_preset(void);
extern const char *const host_crt_names[4];
void host_display_state_pending(unsigned);
/* The GPU the display shows and its renderer mirrors in place of the
 * guest's, when smooth motion draws one of its own; NULL for the guest's. */
void host_display_source(gpu_t *);
/* The renderer mirrors this GPU from now on, starting from its VRAM. */
void host_display_rebind(gpu_t *);
void host_display_recover(gpu_t *, uint64_t);
#ifdef FF8_VULKAN
int host_display_gpu(gpu_t *);
int host_display_scaled_dump(gpu_t *, const char *path);
/* Smooth motion presents on the display's own clock instead of once per guest
 * VBlank: draw gives the GPU to show, drawn for the moment since VBlanks after
 * the guest's last (0 at it) or, whole, as the game shows it now; NULL stops.
 * host_display_smooth_vblank notes each guest VBlank; host_display_tick
 * presents when the window's next image is free, returning 1 when it drew a
 * new picture. While smoothing, host_present leaves the window to the ticks. */
typedef gpu_t *(*host_display_draw_t)(void *context, double since, int whole);
void host_display_smooth(host_display_draw_t draw, void *context);
int host_display_smoothing(void);
void host_display_smooth_vblank(int pal, int whole);
int host_display_tick(void);
/* Called as pacing's wait ends, ms before the guest's next VBlank: draws the
 * next picture first if it would come due while the guest works. */
void host_display_hold(double ms);
/* A CSV of every picture smooth motion presents: when it was for, drawn,
 * presented and seen reaching the display. */
int host_display_present_stats(const char *path);
/* Draws and presents on a thread of its own (on): idle is called there
 * between pictures, to take up what the guest queued. host_display_park holds
 * it still (1) or lets it go (0) while the game thread uses the renderer. */
int host_display_presenter(void (*idle)(void *context), int on);
int host_display_presenting(void);
void host_display_park(int park);
/* Pictures smooth motion has presented. */
uint64_t host_display_pictures(void);
#endif
void host_display_capture(unsigned step, uint64_t first_frame);
void host_display_hotkeys(int off, unsigned discs);
void host_display_disc_mask(unsigned mask);
void host_display_disc(unsigned inserted);
int host_display_present_capture(const char *dir, unsigned width, unsigned height);
int host_display_running(void);
int host_display_failed(void);
int host_input_fast(void);
unsigned host_input_disc(void);
void host_input_dualshock(int enabled);
int host_input_controller(unsigned port);
int32_t host_input_controller_id(unsigned port);
void host_input_axes(unsigned port, uint8_t axes[4]);
void host_input_feedback(unsigned port, sio_pad_output_t, int rumble);
/* Stop feedback during any host pause; focus/minimize/quit also stop it. */
void host_input_pause(int paused);
int host_display_shutdown(gpu_t *);
/* RGB888, tightly packed, including black output when the display is disabled. */
int host_display_rgb(gpu_t *, uint8_t *rgb, size_t capacity, int *width, int *height);
int host_display_dump(gpu_t *, const char *path);
int host_vram_dump(gpu_t *, const char *path);
void host_display_ra(int active, int strict);
#ifdef FF8_GUI
#include "gui.h"
#include "hotkey_bindings.h"
int host_display_gui_attach(void);
void host_display_gui_audio(void (*pause)(void), void (*resume)(void));
host_gui_result_t host_display_gui_loop(int shell);
void host_display_gui_action(unsigned scancode);
unsigned host_display_state_slot_value(void);
unsigned host_display_disc_value(void);
const char *host_display_gui_device(void);
const char *host_display_gui_controller(void);
int host_display_gui_controller_type(void);
int host_display_gui_fullscreen_exclusive(void);
#endif
#endif
