#ifndef FF8_HOST_OPTIONS_H
#define FF8_HOST_OPTIONS_H
#include <stddef.h>
#include <stdint.h>

enum { HOST_SCALE_ASPECT, HOST_SCALE_INTEGER, HOST_SCALE_STRETCH, HOST_SCALE_ASPECT_CROP };
enum { HOST_OPTION_PRESENTATION = 'P', HOST_OPTION_LOGIC = 'L' };
typedef struct {
    int dualshock, rumble, fullscreen, scale_mode, linear;
    int osd, pad_hotkey;
    char screenshot_dir[512], screenshot_at[4096];
    char osd_tests[4096];
    int renderer, true_color, pgxp, full_disc, snapshot_cards, crt, frame_interpolation;
    uint64_t internal_scale, msaa, save_at, present_size;
    char presented_frames[512];
    char movies[512], movie_stats[512], scaled_frames[512], gpu_timings[512];
    char bridge[512], achievements[512], progress[512];
    char state_dir[512], load_state[512], save_state[512];
    char config_path[4096], gui_test[512];
    int gui, no_gui, disc1_configured;
    char discs[4][512], cards[2][512];
#ifdef FF8_PLAYER
    int verify_discs;
#endif
} host_options_t;
enum { HOST_VALUE_ENUM, HOST_VALUE_NUMBER, HOST_VALUE_PATH, HOST_VALUE_BRIDGE, HOST_VALUE_SIZE, HOST_VALUE_OSD_TEST, HOST_VALUE_FRAMES, HOST_VALUE_MEDIA };
enum { HOST_CRT_OFF, HOST_CRT_PVM, HOST_CRT_TV, HOST_CRT_ARCADE };
typedef struct {
    const char *key, *values;
    int classification;
    size_t offset;
    int type;
} host_option_t;
extern const host_option_t host_options[];
extern const size_t host_option_count;
void host_options_defaults(host_options_t *);
/* Load defaults before applying CLI arguments. Missing implicit files are fine. */
int host_options_load(host_options_t *, int argc, char **argv);
/* 1: consumed; 0: not a settings option; -1: invalid/incomplete. */
int host_options_cli(host_options_t *, int argc, char **argv, int *index);
/* GUI and CLI share the same validation; no mutation on invalid input. */
int host_options_set(host_options_t *, const char *key, const char *value);
/* Valid session values survive persistence failure. Atomic, preserving update. */
int host_options_write(host_options_t *, const char *key, const char *value, char *error, size_t);
int host_options_ra_strict(const host_options_t *);
int host_options_ra_validate(const host_options_t *, int fast);
#endif
