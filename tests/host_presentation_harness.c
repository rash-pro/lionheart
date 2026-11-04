#include "display.h"
#include "options.h"
#include <SDL.h>
#include <assert.h>
#include <stdlib.h>
#include <string.h>

static SDL_ScaleMode filter;
static unsigned filter_calls;
static SDL_Window *window;
static int refuse_fullscreen;
static SDL_Rect content, aperture;
int __real_SDL_RenderCopy(SDL_Renderer *, SDL_Texture *, const SDL_Rect *, const SDL_Rect *);
int __wrap_SDL_RenderCopy(SDL_Renderer *renderer, SDL_Texture *texture, const SDL_Rect *src, const SDL_Rect *dest) {
    Uint32 format; assert(!SDL_QueryTexture(texture,&format,NULL,NULL,NULL));
    if (format==SDL_PIXELFORMAT_RGB24) { content = *dest; SDL_RenderGetClipRect(renderer, &aperture); }
    return __real_SDL_RenderCopy(renderer, texture, src, dest);
}
int __real_SDL_SetWindowFullscreen(SDL_Window *, Uint32);
int __wrap_SDL_SetWindowFullscreen(SDL_Window *w, Uint32 flags) {
    if (refuse_fullscreen) return SDL_SetError("synthetic fullscreen failure");
    return __real_SDL_SetWindowFullscreen(w, flags);
}
SDL_Window *__real_SDL_CreateWindow(const char *, int, int, int, int, Uint32);
SDL_Window *__wrap_SDL_CreateWindow(const char *title, int x, int y, int w, int h, Uint32 flags) {
    window = __real_SDL_CreateWindow(title, x, y, w, h, flags); return window;
}
int __real_SDL_SetTextureScaleMode(SDL_Texture *, SDL_ScaleMode);
int __wrap_SDL_SetTextureScaleMode(SDL_Texture *texture, SDL_ScaleMode mode) {
    Uint32 format; assert(!SDL_QueryTexture(texture,&format,NULL,NULL,NULL));
    if (format==SDL_PIXELFORMAT_RGB24) { filter = mode; ++filter_calls; }
    return __real_SDL_SetTextureScaleMode(texture, mode);
}
static void f11(gpu_t *g, int repeat) {
    SDL_Event event; SDL_zero(event); event.type = SDL_KEYDOWN;
    event.key.keysym.scancode = SDL_SCANCODE_F11; event.key.repeat = (Uint8)repeat;
    assert(SDL_PushEvent(&event) == 1); host_present(g);
}
int main(void) {
    gpu_t *g = gpu_create(); assert(g);
    assert(host_display_init(0, NULL, NULL));
    assert(window);
    gpu_gp1(g, 0x06000000 | 600 | (3160u << 12));
    gpu_gp1(g, 0x07000000 | 24 | (248u << 10));
    gpu_gp1(g, 0x08000001);
    gpu_gp1(g, 0x03000000); host_present(g);
    assert(filter_calls && filter == SDL_ScaleModeNearest);
    assert(aperture.w == 960 && aperture.h == 720);
    assert(content.x == 0 && content.y == 24 && content.w == 960 && content.h == 672);
    assert(host_display_configure(1, HOST_SCALE_INTEGER, 1));
    assert(SDL_GetWindowFlags(window) & SDL_WINDOW_FULLSCREEN_DESKTOP);
    host_present(g); assert(filter == SDL_ScaleModeLinear);
    f11(g, 1); assert(SDL_GetWindowFlags(window) & SDL_WINDOW_FULLSCREEN_DESKTOP);
    f11(g, 0); assert(!(SDL_GetWindowFlags(window) & SDL_WINDOW_FULLSCREEN_DESKTOP));
    f11(g, 0); assert(SDL_GetWindowFlags(window) & SDL_WINDOW_FULLSCREEN_DESKTOP);
    {
        const SDL_Scancode keys[] = {SDL_SCANCODE_F5, SDL_SCANCODE_F7, SDL_SCANCODE_F6, SDL_SCANCODE_F8};
        unsigned i;
        for (i = 0; i < 4; ++i) {
            SDL_Event event; SDL_zero(event); event.type = SDL_KEYDOWN; event.key.keysym.scancode = keys[i];
            assert(SDL_PushEvent(&event) == 1); host_present(g);
            assert(host_input_state() == i + 1 && host_input_state() == 0);
            event.key.repeat = 1; assert(SDL_PushEvent(&event) == 1); host_present(g);
            assert(host_input_state() == 0);
        }
        host_display_state_slot(9); assert(strstr(SDL_GetWindowTitle(window), "slot 9"));
        host_display_recover(g, 123); assert(host_input_buttons(0) == 0);
    }
    host_display_ra(1,1);
    assert(strstr(SDL_GetWindowTitle(window)," - RA -"));
    {
        SDL_Event e; SDL_zero(e); e.type=SDL_KEYDOWN;
        e.key.keysym.scancode=SDL_SCANCODE_F7;
        assert(SDL_PushEvent(&e)==1); host_present(g); assert(!host_input_state());
        e.key.keysym.scancode=SDL_SCANCODE_TAB;
        assert(SDL_PushEvent(&e)==1); host_present(g); assert(!host_input_fast());
        e.key.keysym.scancode=SDL_SCANCODE_F5;
        assert(SDL_PushEvent(&e)==1); host_present(g); assert(host_input_state()==1);
        host_display_ra(1,0); assert(strstr(SDL_GetWindowTitle(window),"RA (unrestricted)"));
        e.key.keysym.scancode=SDL_SCANCODE_F7;
        assert(SDL_PushEvent(&e)==1); host_present(g); assert(host_input_state()==2);
        e.key.keysym.scancode=SDL_SCANCODE_TAB;
        assert(SDL_PushEvent(&e)==1); host_present(g); assert(host_input_fast());
        host_display_ra(0,0);
    }
    /* F9 cycles the CRT preset (P) and shows it in the title; strict RA mode never blocks it. */
    {
        static const char *const names[] = {"pvm", "tv", "arcade"};
        SDL_Event e; SDL_zero(e); e.type = SDL_KEYDOWN; e.key.keysym.scancode = SDL_SCANCODE_F9;
        unsigned i;
        char expected[32];
        assert(host_display_crt_preset() == 0 && !strstr(SDL_GetWindowTitle(window), "CRT"));
        host_display_ra(1, 1);
        for (i = 0; i < 3; ++i) {
            assert(SDL_PushEvent(&e) == 1); host_present(g);
            snprintf(expected, sizeof(expected), " - CRT %s", names[i]);
            assert(host_display_crt_preset() == (int)i + 1 && strstr(SDL_GetWindowTitle(window), expected));
            assert(strstr(SDL_GetWindowTitle(window), "State slot 9"));
        }
        e.key.repeat = 1; assert(SDL_PushEvent(&e) == 1); host_present(g);
        assert(host_display_crt_preset() == 3);
        e.key.repeat = 0; assert(SDL_PushEvent(&e) == 1); host_present(g);
        assert(host_display_crt_preset() == 0 && !strstr(SDL_GetWindowTitle(window), "CRT"));
        host_display_ra(0, 0);
        host_display_crt(2); assert(host_display_crt_preset() == 2 && strstr(SDL_GetWindowTitle(window), " - CRT tv"));
        host_display_crt(7); assert(host_display_crt_preset() == 0);
    }
    /* A scanout-size change creates a new texture with the chosen filter. */
    unsigned previous = filter_calls;
    gpu_gp1(g, 0x08000000); host_present(g);
    assert(filter_calls > previous && filter == SDL_ScaleModeLinear);
    assert(host_display_configure(0, HOST_SCALE_STRETCH, 0));
    host_present(g); assert(filter == SDL_ScaleModeNearest && host_display_running());
    assert(!(SDL_GetWindowFlags(window) & SDL_WINDOW_FULLSCREEN_DESKTOP));
    refuse_fullscreen = 1;
    assert(host_display_configure(1, HOST_SCALE_ASPECT, 0));
    f11(g, 0); assert(host_display_running());
    assert(!(SDL_GetWindowFlags(window) & SDL_WINDOW_FULLSCREEN_DESKTOP));
    refuse_fullscreen = 0;
    assert(host_display_shutdown(g)); free(g);
    assert(host_display_init(1, NULL, NULL));
    assert(host_display_configure(1, HOST_SCALE_INTEGER, 1));
    assert(host_display_shutdown(NULL)); return 0;
}
