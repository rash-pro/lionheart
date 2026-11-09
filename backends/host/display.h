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
void host_display_recover(gpu_t *, uint64_t);
#ifdef FF8_VULKAN
int host_display_gpu(gpu_t *);
int host_display_scaled_dump(gpu_t *, const char *path);
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
