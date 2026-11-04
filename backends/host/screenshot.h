#ifndef FF8_HOST_SCREENSHOT_H
#define FF8_HOST_SCREENSHOT_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
/* One pending request, including the worker's encode/write. Only the game
 * thread calls these. read runs on the worker; release runs on the game
 * thread after completion, before the presenter/device can be destroyed. */
enum { HOST_SCREENSHOT_RGB, HOST_SCREENSHOT_RGBA, HOST_SCREENSHOT_BGRA };
int host_screenshot_configure(const char *directory, const char *frames);
void host_screenshot_request(void);
void host_screenshot_frame(uint64_t frame);
int host_screenshot_pending(void);
void host_screenshot_submit(int w, int h, int format, void *data,
                            const uint8_t *(*read)(void *), void (*release)(void *));
void host_screenshot_fail(const char *reason);
void host_screenshot_flush(void);
void host_screenshot_shutdown(void);
int host_screenshot_directory(char *out, unsigned size);
int host_png(const char *path, const uint8_t *rgb, int w, int h);
#ifdef __cplusplus
}
#endif
#endif
