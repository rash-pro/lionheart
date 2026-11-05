#ifndef FF8_HOST_OSD_H
#define FF8_HOST_OSD_H
#include "viewport.h"
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
enum {
    HOST_OSD_CRT, HOST_OSD_CRT_SOFT, HOST_OSD_SLOT, HOST_OSD_SAVING, HOST_OSD_SAVED,
    HOST_OSD_SAVE_FAILED, HOST_OSD_LOADING, HOST_OSD_LOADED, HOST_OSD_NO_STATE,
    HOST_OSD_LOAD_FAILED, HOST_OSD_CARD, HOST_OSD_RA_LOAD, HOST_OSD_RA_FAST,
    HOST_OSD_FAST, HOST_OSD_DISC, HOST_OSD_CONNECTED, HOST_OSD_DISCONNECTED,
    HOST_OSD_UNAVAILABLE, HOST_OSD_SCREENSHOT, HOST_OSD_SCREENSHOT_FAILED
};
typedef struct {
    const uint8_t *rgba;
    int x, y, w, h, cell_w, cell_h;
    unsigned revision;
    uint8_t opacity;
} host_osd_panel_t;
void host_osd_configure(int enabled, const char *tests);
/* Called once per presentation, independently of restored guest VBlanks. */
void host_osd_frame(int window, uint64_t frame);
/* Controllable milliseconds for unit tests and offscreen preview tools. */
void host_osd_clock(double milliseconds);
double host_osd_now(void);
float host_osd_corner_radius(int height);
/* Switch to wall time during a host menu without advancing diagnostic frames. */
void host_osd_pause(int paused);
void host_osd_post(const char *label, const char *value, int persistent);
void host_osd_event(unsigned event, unsigned number, const char *detail);
void host_osd_clear(unsigned event);
/* The visible picture: the scanout clipped to the TV aperture (frame when empty). */
host_rect_t host_osd_area(host_viewport_t view);
const host_osd_panel_t *host_osd_panel(int width, int height, host_rect_t area);
void host_osd_shutdown(void);
#ifdef __cplusplus
}
#endif
#endif
