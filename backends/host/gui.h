#ifndef FF8_HOST_GUI_H
#define FF8_HOST_GUI_H
#include "options.h"
#include <SDL.h>
#ifdef __cplusplus
extern "C" {
#endif

typedef enum { HOST_GUI_HIDDEN, HOST_GUI_SETUP, HOST_GUI_HUB, HOST_GUI_OVERLAY } host_gui_mode_t;
typedef enum { HOST_GUI_GAME, HOST_GUI_DISCS, HOST_GUI_CARD, HOST_GUI_VIDEO, HOST_GUI_CONTROLS, HOST_GUI_RA } host_gui_pane_t;
typedef enum { HOST_GUI_RUNNING, HOST_GUI_PLAY, HOST_GUI_QUIT, HOST_GUI_RECREATE, HOST_GUI_ERROR } host_gui_result_t;
typedef enum { HOST_GUI_KEYBOARD, HOST_GUI_MOUSE, HOST_GUI_PAD } host_gui_input_t;
typedef struct {
    void *context;
    void (*changed)(void *, const char *key, int overlay);
    void (*action)(void *, unsigned sdl_scancode);
    unsigned (*state_slot)(void *);
    int (*state_available)(void *, unsigned slot, char *date, size_t);
    unsigned (*disc)(void *);
    host_gui_result_t (*loop)(void *, int shell);
    const char *(*device)(void *);
    const char *(*controller)(void *);
    int (*controller_type)(void *);
    int (*fullscreen_exclusive)(void *);
} host_gui_services_t;
int host_gui_configure(host_options_t *, const char *data_dir, const host_gui_services_t *, const char *script);
void host_gui_attach_game(void);
void host_gui_startup_error(host_gui_pane_t,const char *detail);
void host_gui_hub(void);
host_gui_result_t host_gui_shell(void);
void host_gui_request(host_gui_pane_t);
int host_gui_requested(void);
void host_gui_open(void);
int host_gui_visible(void);
int host_gui_enabled(void);
host_gui_mode_t host_gui_mode(void);
host_gui_pane_t host_gui_pane(void);
host_gui_input_t host_gui_last_input(void);
void host_gui_frame(unsigned width, unsigned height);
void host_gui_event(const SDL_Event *);
host_gui_result_t host_gui_result(void);
void host_gui_close(void);
void host_gui_shutdown(void);
const char *host_gui_string(const char *id);
/* Focused ImGui text fields register their caller-owned buffer for the pad keyboard. */
void host_gui_edit_focus(char *text, size_t capacity, int password);
/* Script driver implemented beside the UI adapter. */
int host_gui_test_load(const char *path);
void host_gui_test_tick(uint64_t guest_frame);
void host_gui_test_window(SDL_Window *);
void host_gui_test_event(SDL_Event *);
void host_gui_test_observe(void);
void host_gui_test_timing(double build_ms,double present_ms,double frame_ms);
void host_gui_test_gpu_timing(double pass_ms);
const char *host_gui_test_capture(void);
void host_gui_test_captured(int success);
int host_gui_test_failed(void);
int host_gui_test_active(void);
int host_gui_test_finished(void);
void host_gui_test_shutdown(void);
#ifdef __cplusplus
}
#endif
#endif
