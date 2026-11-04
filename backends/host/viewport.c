#include "viewport.h"
#include <stdint.h>

static host_rect_t fit(int64_t numerator, int64_t denominator, int rw, int rh) {
    host_rect_t r;
    if ((int64_t)rw * denominator > (int64_t)rh * numerator) {
        r.h = rh; r.w = (int)((int64_t)rh * numerator / denominator);
    } else {
        r.w = rw; r.h = (int)((int64_t)rw * denominator / numerator);
    }
    r.x = (rw - r.w) / 2; r.y = (rh - r.h) / 2;
    return r;
}

/* Round edges independently, including content that starts outside the TV. */
static int edge(int units, int pixels, int span) {
    int64_t n = (int64_t)units * pixels;
    return (int)(n < 0 ? -((-n + span / 2) / span) : (n + span / 2) / span);
}

host_viewport_t host_viewport(gpu_scanout_t s, int width, int height, int rw, int rh, int mode) {
    /* psx-spx GP1(06h) fullscreen timing, indexed by GP1(08h)'s dot clock.
     * The 7-clock mode's full TV aperture spans 384 pixels, not its usual 368. */
    static const int start[2][5] = {{590, 600, 615, 620, 539}, {610, 624, 635, 640, 560}};
    static const int divider[5] = {10, 8, 5, 4, 7};
    unsigned clock = s.mode & 64 ? 4 : s.mode & 3;
    int pal = !!(s.mode & 8), repeat = (s.mode & 36) == 36 ? 2 : 1;
    int full_rows = (pal ? 288 : 240) * repeat, span = clock == 4 ? 2688 : 2560;
    int x = (int)(s.horizontal & 4095) - start[pal][clock];
    int y = ((int)(s.vertical & 1023) - (pal ? 19 : 16)) * repeat;
    int right, bottom;
    host_viewport_t out = {{0, 0, 0, 0}, {0, 0, 0, 0}};
    if (rw <= 0 || rh <= 0 || width <= 0 || height <= 0 ||
        (s.horizontal >> 12) <= (s.horizontal & 4095) ||
        (s.vertical >> 10) <= (s.vertical & 1023)) return out;
    if (mode == HOST_SCALE_STRETCH) {
        out.frame.w = rw; out.frame.h = rh; out.content = out.frame; return out;
    }
    if (mode == HOST_SCALE_ASPECT_CROP) {
        out.frame = fit((int64_t)width * divider[clock] * full_rows * 4,
                        (int64_t)height * span * 3, rw, rh);
        out.content = out.frame; return out;
    }
    out.frame = fit(4, 3, rw, rh);
    if (mode == HOST_SCALE_INTEGER) {
        int scale = out.frame.h / full_rows;
        if (scale) {
            out.frame.h = full_rows * scale; out.frame.w = out.frame.h * 4 / 3;
            out.frame.x = (rw - out.frame.w) / 2; out.frame.y = (rh - out.frame.h) / 2;
        }
    }
    right = edge(x + width * divider[clock], out.frame.w, span);
    bottom = edge(y + height, out.frame.h, full_rows);
    out.content.x = out.frame.x + edge(x, out.frame.w, span);
    out.content.y = out.frame.y + edge(y, out.frame.h, full_rows);
    out.content.w = right - edge(x, out.frame.w, span);
    out.content.h = bottom - edge(y, out.frame.h, full_rows);
    return out;
}
