#ifndef FF8_HOST_GUI_H
#define FF8_HOST_GUI_H
#include "options.h"
#include <SDL.h>
#ifdef __cplusplus
extern "C" {
#endif

typedef enum { HOST_GUI_HIDDEN, HOST_GUI_SETUP, HOST_GUI_HUB, HOST_GUI_OVERLAY } host_gui_mode_t;
typedef enum { HOST_GUI_GAME, HOST_GUI_DISCS, HOST_GUI_CARD, HOST_GUI_VIDEO, HOST_GUI_CONTROLS, HOST_GUI_RA, HOST_GUI_ACHIEVEMENTS } host_gui_pane_t;
typedef enum { HOST_GUI_RUNNING, HOST_GUI_PLAY, HOST_GUI_QUIT, HOST_GUI_RECREATE, HOST_GUI_ERROR } host_gui_result_t;
typedef enum { HOST_GUI_KEYBOARD, HOST_GUI_MOUSE, HOST_GUI_PAD } host_gui_input_t;
/* Native code as the player knows it this frame. */
typedef enum { HOST_GUI_COMPILER_MISSING, HOST_GUI_COMPILER_INSTALLING, HOST_GUI_COMPILER_FAILED, HOST_GUI_COMPILER_READY } host_gui_compiler_t;
typedef enum { HOST_GUI_BUILD_OFF, HOST_GUI_BUILD_RUNNING, HOST_GUI_BUILD_DONE, HOST_GUI_BUILD_STOPPED } host_gui_build_t;
typedef enum { HOST_GUI_STEP_DOWNLOAD, HOST_GUI_STEP_VERIFY, HOST_GUI_STEP_EXTRACT, HOST_GUI_STEP_CHECK, HOST_GUI_STEP_INSTALL } host_gui_step_t;
/* A compiler the system has, which leaves setup nothing to ask. */
typedef enum { HOST_GUI_SYSTEM_NONE, HOST_GUI_SYSTEM_GCC, HOST_GUI_SYSTEM_CLANG } host_gui_system_t;
typedef struct {
    host_gui_compiler_t compiler;
    /* While installing: the step and how far it is, in bytes or entries. */
    host_gui_step_t step;
    unsigned long long done,total;
    host_gui_system_t system;
    char version[32];
    /* The download: its size, whether this build has an address for it, the
     * archive's kind ("zip", "tar.xz") and its file name when the address
     * gives one. */
    unsigned long long bytes;
    int downloadable;
    char format[16],archive[128],reason[256];
    host_gui_build_t build;
    unsigned sets,ready,failed;
    /* -1 while there is no estimate worth showing. */
    int seconds_left;
    char error[256];
} host_gui_native_t;
typedef struct { char name[32],words[512]; } host_gui_failure_t;
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
    /* Absent in hosts without native code; the interface then shows none. */
    void (*native)(void *, host_gui_native_t *);
    /* archive NULL downloads. */
    void (*native_install)(void *, const char *archive);
    void (*native_cancel)(void *);
    /* Fills up to capacity rows; returns how many sets failed. */
    unsigned (*native_failures)(void *, host_gui_failure_t *, unsigned capacity);
} host_gui_services_t;
int host_gui_configure(host_options_t *, const char *data_dir, const host_gui_services_t *, const char *script);
void host_gui_attach_game(void);
void host_gui_startup_error(host_gui_pane_t,const char *detail);
void host_gui_hub(void);
/* The previous session ended without saying so; its log is at path. */
void host_gui_previous_crash(const char *log);
/* Something must be shown before the game starts: the compiler step to an
 * installation that finished setup without it, or a crash. */
int host_gui_wants_shell(void);
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
