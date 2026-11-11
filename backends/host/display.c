#include "identity.h"
#include "display.h"
#ifdef FF8_VULKAN
#include "vulkan_renderer.h"
#include "present_pace.h"
#endif
#include "sio.h"
#include "viewport.h"
#include "osd.h"
#include "screenshot.h"
#include "hotkey_bindings.h"
#ifdef FF8_GUI
#include "gui_internal.h"
#endif
#include <SDL.h>
#include <errno.h>
#include <math.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static struct {
    SDL_Window *window;
    SDL_Renderer *renderer;
    SDL_Texture *texture, *osd_texture;
    SDL_Surface *surface;
    const char *presented;
    unsigned present_w, present_h;
    unsigned osd_revision;
    uint64_t osd_frame;
    int initialized, video, width, height, quit, failed;
    unsigned long long frame;
    unsigned capture_step;
    uint64_t capture_from;
    const char *frames, *vram_path;
    uint8_t keys[SDL_NUM_SCANCODES];
    unsigned disc_request, state_action, state_slot, state_pending;
    SDL_GameController *controllers[2];
    SDL_JoystickID controller_ids[2];
    uint16_t controller_buttons[2], controller_axes[2], consumed[2];
    uint8_t guide[2], pad_fast[2], guide_chord[2];
    int hotkey_off;
    unsigned discs, disc_mask, inserted;
    uint8_t sticks[2][4];
    int led_state[2];
    int dualshock, focused, paused;
    int ra_active, ra_strict;
    int scale_mode, linear, crt, crt_warned;
    gpu_t *source;
#ifdef FF8_GUI
    int gui_attached;
    void (*audio_pause)(void), (*audio_resume)(void);
    gpu_scanout_t retained_timing;
#endif
    uint8_t rgb[1024 * 512 * 3];
} host;

const char *const host_crt_names[4] = {"off", "pvm", "tv", "arcade"};
static void fullscreen(int enabled) {
    if (host.window && SDL_SetWindowFullscreen(host.window, enabled ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0)) {
        fprintf(stderr, "Display fullscreen warning: %s (keeping current mode)\n", SDL_GetError());
    }
}
int host_display_configure(int full, int scale_mode, int linear) {
    host.scale_mode = scale_mode; host.linear = linear;
#ifdef FF8_VULKAN
    host_gpu_presentation(scale_mode,linear);
#endif
    if (host.texture && SDL_SetTextureScaleMode(host.texture, linear ? SDL_ScaleModeLinear : SDL_ScaleModeNearest)) return 0;
    fullscreen(full); return 1;
}

void host_input_dualshock(int enabled) {
    host.dualshock = enabled;
    host.led_state[0] = host.led_state[1] = -1;
}
int host_input_controller(unsigned port) { return port < 2 && host.controllers[port] != NULL; }
int32_t host_input_controller_id(unsigned port) {
    return host_input_controller(port) ? host.controller_ids[port] : -1;
}
void host_input_axes(unsigned port, uint8_t axes[4]) {
    memset(axes, 0x80, 4);
    if (port < 2 && host.dualshock && !host.paused) memcpy(axes, host.sticks[port], 4);
}
void host_input_feedback(unsigned port, sio_pad_output_t out, int rumble) {
    if (port < 2 && host.controllers[port]) {
        int active = rumble && host.focused && !host.paused && !host.quit;
        SDL_GameControllerRumble(host.controllers[port], active ? out.large_motor * 257u : 0,
                                 active && out.small_motor ? 65535 : 0, 100);
        if (host.dualshock && host.led_state[port] != out.analog && SDL_GameControllerHasLED(host.controllers[port])) {
            SDL_GameControllerSetLED(host.controllers[port], out.analog ? 255 : 0, 0, 0);
            host.led_state[port] = out.analog;
        }
    }
}
static void stop_feedback(void) {
    unsigned port;
    for (port = 0; port < 2; ++port) if (host.controllers[port])
        SDL_GameControllerRumble(host.controllers[port], 0, 0, 100);
}
void host_input_pause(int paused) {
    host.paused = paused;
    if (paused) stop_feedback();
}

static uint16_t keyboard_button(SDL_Scancode key) {
    switch (key) {
    case SDL_SCANCODE_UP: case SDL_SCANCODE_W: return PAD_UP;
    case SDL_SCANCODE_RIGHT: case SDL_SCANCODE_D: return PAD_RIGHT;
    case SDL_SCANCODE_DOWN: case SDL_SCANCODE_S: return PAD_DOWN;
    case SDL_SCANCODE_LEFT: case SDL_SCANCODE_A: return PAD_LEFT;
    case SDL_SCANCODE_X: return PAD_CROSS;
    case SDL_SCANCODE_Z: return PAD_CIRCLE;
    case SDL_SCANCODE_C: return PAD_SQUARE;
    case SDL_SCANCODE_V: return PAD_TRIANGLE;
    case SDL_SCANCODE_Q: return PAD_L1;
    case SDL_SCANCODE_E: return PAD_R1;
    case SDL_SCANCODE_1: return PAD_L2;
    case SDL_SCANCODE_3: return PAD_R2;
    case SDL_SCANCODE_RETURN: return PAD_START;
    case SDL_SCANCODE_BACKSPACE: return PAD_SELECT;
    default: return 0;
    }
}

static uint16_t controller_button(unsigned button) {
    switch (button) {
    case SDL_CONTROLLER_BUTTON_A: return PAD_CROSS;
    case SDL_CONTROLLER_BUTTON_B: return PAD_CIRCLE;
    case SDL_CONTROLLER_BUTTON_X: return PAD_SQUARE;
    case SDL_CONTROLLER_BUTTON_Y: return PAD_TRIANGLE;
    case SDL_CONTROLLER_BUTTON_BACK: return PAD_SELECT;
    case SDL_CONTROLLER_BUTTON_START: return PAD_START;
    case SDL_CONTROLLER_BUTTON_LEFTSTICK: return PAD_L3;
    case SDL_CONTROLLER_BUTTON_RIGHTSTICK: return PAD_R3;
    case SDL_CONTROLLER_BUTTON_LEFTSHOULDER: return PAD_L1;
    case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER: return PAD_R1;
    case SDL_CONTROLLER_BUTTON_DPAD_UP: return PAD_UP;
    case SDL_CONTROLLER_BUTTON_DPAD_DOWN: return PAD_DOWN;
    case SDL_CONTROLLER_BUTTON_DPAD_LEFT: return PAD_LEFT;
    case SDL_CONTROLLER_BUTTON_DPAD_RIGHT: return PAD_RIGHT;
    default: return 0;
    }
}

uint16_t host_input_buttons(unsigned port) {
    uint16_t pressed;
    unsigned key;
    if (port >= 2 || host.paused) return 0;
    pressed = (host.controller_buttons[port] & (uint16_t)~host.consumed[port]) |
        (host.controller_axes[port] & (uint16_t)~(host.consumed[port] & (PAD_L2|PAD_R2)));
    if (!port) for (key = 0; key < SDL_NUM_SCANCODES; ++key)
        if (host.keys[key]) pressed |= keyboard_button((SDL_Scancode)key);
    return pressed;
}

int host_input_fast(void) { return !host.ra_strict && (host.keys[SDL_SCANCODE_TAB] || host.pad_fast[0] || host.pad_fast[1]); }

unsigned host_input_disc(void) {
    unsigned disc = host.disc_request;
    host.disc_request = 0;
    if (disc) host_osd_event(HOST_OSD_DISC,disc,NULL);
    return disc;
}

unsigned host_input_state(void) {
    unsigned action = host.state_action; host.state_action = 0; return action;
}
void host_display_state_slot(unsigned slot) {
    char title[128], crt[24] = "";
    host.state_slot = slot;
    if (host.crt) snprintf(crt, sizeof(crt), " - CRT %s", host_crt_names[host.crt]);
    snprintf(title,sizeof(title),HOST_TITLE "%s - State slot %u%s%s",host.ra_active ? (host.ra_strict ? " - RA" : " - RA (unrestricted)") : "",slot,
             host.state_pending == 1 ? " - saving\xe2\x80\xa6" : host.state_pending == 2 ? " - loading\xe2\x80\xa6" : "", crt);
    if (host.window) SDL_SetWindowTitle(host.window,title);
}
/* P: the preset only changes presentation; F9 cycles it and RA mode never blocks it. */
void host_display_crt(int preset) {
    host.crt = preset >= 0 && preset < 4 ? preset : 0;
#ifdef FF8_VULKAN
    host_gpu_crt(host.crt);
#endif
    host_display_state_slot(host.state_slot);
}
int host_display_crt_preset(void) { return host.crt; }
void host_display_ra(int active, int strict) {
    host.ra_active=active; host.ra_strict=strict; host_display_state_slot(host.state_slot);
}
void host_display_state_pending(unsigned action) {
    host.state_pending = action; host_display_state_slot(host.state_slot);
}
void host_display_hotkeys(int off,unsigned discs) {
    host.hotkey_off=off; host.discs=discs; host.inserted=discs ? 1 : 0;
    host.disc_mask=(1u<<discs)-1;
}
void host_display_disc_mask(unsigned mask) { host.disc_mask=mask & 15; }
void host_display_disc(unsigned inserted) { host.inserted=inserted; }
static int action(unsigned key,int down) {
    if (host.ra_strict && down && (key==SDL_SCANCODE_F7 || key==SDL_SCANCODE_TAB)) {
        fprintf(stderr,"RA mode: blocked %s\n",key==SDL_SCANCODE_F7 ? "save-state loading (F7)" : "fast-forward (Tab)");
        host_osd_event(key==SDL_SCANCODE_F7 ? HOST_OSD_RA_LOAD : HOST_OSD_RA_FAST,0,NULL);
        return 0;
    }
    if (key==SDL_SCANCODE_TAB) {
        if (down) host_osd_event(HOST_OSD_FAST,0,NULL);
        else if (!host_input_fast()) host_osd_clear(HOST_OSD_FAST);
    }
    if (!down) return 1;
#ifdef FF8_GUI
    if (key==SDL_SCANCODE_F10 && host_gui_enabled()) host_gui_request(HOST_GUI_ACHIEVEMENTS);
#endif
    if (key>=SDL_SCANCODE_F1 && key<=SDL_SCANCODE_F4) host.disc_request=key-SDL_SCANCODE_F1+1;
    if (key==SDL_SCANCODE_F5) host.state_action=1;
    if (key==SDL_SCANCODE_F7) host.state_action=2;
    if (key==SDL_SCANCODE_F6) host.state_action=3;
    if (key==SDL_SCANCODE_F8) host.state_action=4;
    if (key==SDL_SCANCODE_F11 && host.window) fullscreen(!(SDL_GetWindowFlags(host.window)&SDL_WINDOW_FULLSCREEN_DESKTOP));
    if (key==SDL_SCANCODE_F12) host_screenshot_request();
    if (key==SDL_SCANCODE_F9) {
        host_display_crt((host.crt+1)%4);
#ifdef FF8_VULKAN
        host_osd_event(host_gpu_enabled() ? HOST_OSD_CRT : HOST_OSD_CRT_SOFT,host.crt,NULL);
#else
        host_osd_event(HOST_OSD_CRT_SOFT,host.crt,NULL);
#endif
    }
    return 1;
}
static void pad_button(unsigned port,uint16_t bit,int down,int axis) {
    uint16_t *held=axis ? &host.controller_axes[port] : &host.controller_buttons[port];
    if (!down) {
        *held&=(uint16_t)~bit;
        host.consumed[port]&=(uint16_t)~bit;
        if (bit==PAD_R2 && host.pad_fast[port]) { host.pad_fast[port]=0; action(SDL_SCANCODE_TAB,0); }
    } else if (!(*held&bit)) {
        *held|=bit;
        if (host.guide[port]) host.guide_chord[port]=1;
        if (host.guide[port] && !host.hotkey_off) {
            unsigned key=0;
            host.guide_chord[port]=1;
            host.consumed[port]|=bit;
            for (unsigned n=0;n<sizeof(host_chords)/sizeof(host_chords[0]);++n)
                if (host_chords[n].pad_button==bit) { key=host_chords[n].scancode; break; }
            if (bit==PAD_SELECT) {
                key=0;
                if (host.discs) for (unsigned n=0;n<4;++n) {
                    unsigned disc=(host.inserted+n)%4;
                    if (host.disc_mask&(1u<<disc)) { key=SDL_SCANCODE_F1+disc; break; }
                    char message[64];
                    snprintf(message,sizeof(message),"Disc %u missing",disc+1);
                    host_osd_post(message,NULL,1);
                    fprintf(stderr,"%s; skipping empty slot\n",message);
                }
            }
            if (key && action(key,1) && bit==PAD_R2) host.pad_fast[port]=1;
        }
    }
}
static void clear_pad(void) {
    memset(host.controller_buttons,0,sizeof(host.controller_buttons));
    memset(host.controller_axes,0,sizeof(host.controller_axes));
    memset(host.consumed,0,sizeof(host.consumed));
    memset(host.guide,0,sizeof(host.guide));
    memset(host.guide_chord,0,sizeof(host.guide_chord));
    memset(host.pad_fast,0,sizeof(host.pad_fast));
    memset(host.sticks,0x80,sizeof(host.sticks));
}
static void input_event(const SDL_Event *event) {
    unsigned port;
    if (event->type == SDL_KEYDOWN || event->type == SDL_KEYUP) {
        unsigned key=(unsigned)event->key.keysym.scancode;
        int down=event->type==SDL_KEYDOWN;
        if (!down && key<SDL_NUM_SCANCODES) host.keys[key]=0;
        if (host.ra_strict && down && event->key.repeat && (key==SDL_SCANCODE_F7 || key==SDL_SCANCODE_TAB)) return;
        if (!event->key.repeat && !action(key,down)) return;
        if (key<SDL_NUM_SCANCODES) host.keys[key]=down;
    } else if (event->type == SDL_WINDOWEVENT &&
               (event->window.event == SDL_WINDOWEVENT_FOCUS_LOST ||
                event->window.event == SDL_WINDOWEVENT_MINIMIZED || event->window.event == SDL_WINDOWEVENT_HIDDEN)) {
        host_osd_clear(HOST_OSD_FAST);
        host.focused = 0;
        stop_feedback();
        memset(host.keys, 0, sizeof(host.keys));
        clear_pad();
    } else if (event->type == SDL_WINDOWEVENT && event->window.event == SDL_WINDOWEVENT_FOCUS_GAINED) {
        host.focused = 1;
    } else if (event->type == SDL_CONTROLLERDEVICEADDED) {
        host_osd_event(HOST_OSD_CONNECTED,0,SDL_GameControllerNameForIndex(event->cdevice.which));
        for (port = 0; port < 2; ++port) if (!host.controllers[port]) {
            host.controllers[port] = SDL_GameControllerOpen(event->cdevice.which);
            if (host.controllers[port]) {
                host.controller_ids[port] = SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(host.controllers[port]));
            }
            host.led_state[port] = -1;
            break;
        }
    } else if (event->type == SDL_CONTROLLERDEVICEREMOVED) {
        host_osd_event(HOST_OSD_DISCONNECTED,0,NULL);
        for (port = 0; port < 2; ++port)
            if (host.controllers[port] && host.controller_ids[port] == event->cdevice.which) {
                SDL_GameControllerRumble(host.controllers[port], 0, 0, 100);
                SDL_GameControllerClose(host.controllers[port]); host.controllers[port] = NULL;
                host.controller_buttons[port] = host.controller_axes[port] = host.consumed[port] = 0;
                host.guide[port]=host.pad_fast[port]=0;
                if (!host_input_fast()) host_osd_clear(HOST_OSD_FAST);
                memset(host.sticks[port], 0x80, 4);
            }
    } else if (event->type == SDL_CONTROLLERBUTTONDOWN || event->type == SDL_CONTROLLERBUTTONUP) {
        for (port = 0; port < 2; ++port)
            if (host.controllers[port] && host.controller_ids[port] == event->cbutton.which) {
                uint16_t bit = controller_button(event->cbutton.button);
                if (event->cbutton.button==SDL_CONTROLLER_BUTTON_GUIDE) {
#ifdef FF8_GUI
                    if (event->type==SDL_CONTROLLERBUTTONDOWN)
                        host.guide_chord[port]=(host.controller_buttons[port] || host.controller_axes[port]);
                    else if (host.guide[port] && !host.guide_chord[port] && host_gui_enabled()) host_gui_request(HOST_GUI_GAME);
#endif
                    host.guide[port]=event->type==SDL_CONTROLLERBUTTONDOWN;
                }
                else pad_button(port,bit,event->type==SDL_CONTROLLERBUTTONDOWN,0);
            }
    } else if (event->type == SDL_CONTROLLERAXISMOTION) {
        for (port = 0; port < 2; ++port)
            if (host.controllers[port] && host.controller_ids[port] == event->caxis.which) {
                uint16_t mask = 0, bits = 0;
                int value = event->caxis.value;
                if (event->caxis.axis <= SDL_CONTROLLER_AXIS_RIGHTY) {
                    unsigned axis = event->caxis.axis;
                    host.sticks[port][axis < 2 ? axis + 2 : axis - 2] = (uint8_t)((value + 32768) / 256);
                }
                switch (event->caxis.axis) {
                case SDL_CONTROLLER_AXIS_LEFTX:
                    mask = PAD_LEFT | PAD_RIGHT; bits = host.dualshock ? 0 : value < -16000 ? PAD_LEFT : value > 16000 ? PAD_RIGHT : 0; break;
                case SDL_CONTROLLER_AXIS_LEFTY:
                    mask = PAD_UP | PAD_DOWN; bits = host.dualshock ? 0 : value < -16000 ? PAD_UP : value > 16000 ? PAD_DOWN : 0; break;
                case SDL_CONTROLLER_AXIS_TRIGGERLEFT: mask = PAD_L2; bits = value > 16000 ? PAD_L2 : 0; break;
                case SDL_CONTROLLER_AXIS_TRIGGERRIGHT: mask = PAD_R2; bits = value > 16000 ? PAD_R2 : 0; break;
                default: break;
                }
                if (mask&(PAD_L2|PAD_R2)) pad_button(port,mask,bits!=0,1);
                else host.controller_axes[port] = (host.controller_axes[port] & (uint16_t)~mask) | bits;
            }
    }
}

static uint8_t expand5(unsigned x) { return (uint8_t)((x << 3) | (x >> 2)); }
static void rgb15(uint16_t value, uint8_t *out) {
    out[0] = expand5(value & 31); out[1] = expand5((value >> 5) & 31); out[2] = expand5((value >> 10) & 31);
}

int host_display_rgb(gpu_t *g, uint8_t *rgb, size_t capacity, int *w, int *h) {
    int x, y, depth, enabled, i, j;
    const uint16_t *vram = gpu_vram(g);
    gpu_display(g, &x, &y, w, h, &depth, &enabled);
    if (capacity < (size_t)*w * (size_t)*h * 3) return 0;
    if (!enabled) { memset(rgb, 0, (size_t)*w * (size_t)*h * 3); return 1; }
    for (j = 0; j < *h; ++j) for (i = 0; i < *w; ++i) {
        uint8_t *out = rgb + ((size_t)j * *w + i) * 3;
        unsigned row = ((unsigned)(y + j) & 511) * 1024;
        if (depth) {
            unsigned k, at = (unsigned)x * 2 + (unsigned)i * 3;
            for (k = 0; k < 3; ++k) {
                unsigned byte = (at + k) & 2047;
                out[k] = (uint8_t)(vram[row + byte / 2] >> ((byte & 1) * 8));
            }
        } else rgb15(vram[row + ((x + i) & 1023)], out);
    }
    return 1;
}


int host_display_dump(gpu_t *g, const char *path) {
    int w, h;
    if (!host_display_rgb(g, host.rgb, sizeof(host.rgb), &w, &h)) return 0;
    if (!w || !h) { w = h = 1; memset(host.rgb, 0, 3); }
    return host_png(path, host.rgb, w, h);
}

int host_vram_dump(gpu_t *g, const char *path) {
    const uint16_t *vram = gpu_vram(g);
    unsigned i;
    for (i = 0; i < 1024 * 512; ++i) rgb15(vram[i], host.rgb + i * 3);
    return host_png(path, host.rgb, 1024, 512);
}

static int make_directory(const char *path) {
    size_t i, n = strlen(path);
    char *copy = malloc(n + 1);
    struct stat status;
    int ok = 1;
    if (!copy || !n) { free(copy); return 0; }
    memcpy(copy, path, n + 1);
    for (i = 1; i <= n; ++i) if (copy[i] == '/' || !copy[i]) {
        char saved = copy[i]; copy[i] = 0;
        if (mkdir(copy, 0777) && errno != EEXIST) ok = 0;
        if (stat(copy, &status) || !S_ISDIR(status.st_mode)) ok = 0;
        copy[i] = saved;
        if (!ok) break;
    }
    free(copy);
    return ok;
}

static void fail(const char *message) {
    fprintf(stderr, "Display: %s\n", message); host.failed = host.quit = 1;
}

static int soft_renderer(void) {
    /* Prefer desktop GL on Wayland without forcing that choice on the software fallback. */
    int driver = -1;
    const char *video = SDL_GetCurrentVideoDriver();
    if (video && !strcmp(video, "wayland") && !SDL_GetHint(SDL_HINT_RENDER_DRIVER)) {
        int i;
        for (i = 0; i < SDL_GetNumRenderDrivers(); ++i) {
            SDL_RendererInfo info;
            if (!SDL_GetRenderDriverInfo(i, &info) && !strcmp(info.name, "opengl")) {
                driver = i; break;
            }
        }
    }
    host.renderer = SDL_CreateRenderer(host.window, driver, SDL_RENDERER_ACCELERATED);
    if (!host.renderer) host.renderer = SDL_CreateRenderer(host.window, -1, SDL_RENDERER_SOFTWARE);
    if (!host.renderer) { fail(SDL_GetError()); return 0; }
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "nearest");
    return 1;
}
#ifdef FF8_VULKAN
static int soft_fallback(void) {
    if (host_gpu_enabled()) return 1;
    host_gpu_stop();
    if (!host.video || host.renderer) return 1;
#ifdef FF8_GUI
    if (!host.window && host_gui_test_active()) return 1;
#endif
    if (host.window && (SDL_GetWindowFlags(host.window)&SDL_WINDOW_VULKAN)) {
        int x,y,w,h;
        SDL_GetWindowPosition(host.window,&x,&y); SDL_GetWindowSize(host.window,&w,&h);
        SDL_DestroyWindow(host.window);
        host.window=SDL_CreateWindow(HOST_TITLE,x,y,w,h,SDL_WINDOW_RESIZABLE|SDL_WINDOW_ALLOW_HIGHDPI);
        if (!host.window) { fail("Vulkan failed and a soft window could not be created"); return 0; }
    }
    return soft_renderer();
}
#endif

int host_display_init(int headless, const char *frames, const char *vram_path) {
    if (host.initialized) return 0;
    memset(&host, 0, sizeof(host));
    host.initialized = 1; host.frames = frames; host.vram_path = vram_path;
    host.focused = 1; host.present_w=1920; host.present_h=1080;
    memset(host.sticks, 0x80, sizeof(host.sticks));
    if (frames && !make_directory(frames)) { fail("cannot create frame dump directory"); return 0; }
#ifdef FF8_VULKAN
    if (host_gpu_scaled_frames() && !make_directory(host_gpu_scaled_frames())) { fail("cannot create scaled dump directory"); return 0; }
    if (host_gpu_presented_frames() && !make_directory(host_gpu_presented_frames())) { fail("cannot create presented dump directory"); return 0; }
#endif
    if (headless) return 1;
    if (SDL_InitSubSystem(SDL_INIT_VIDEO)) { fail(SDL_GetError()); return 0; }
    host.video = 1;
    if (SDL_InitSubSystem(SDL_INIT_GAMECONTROLLER)) { fail(SDL_GetError()); return 0; }
#ifdef FF8_GUI
    /* Scripted shell diagnostics use the same offscreen presenters as captures;
     * --headless continues to bypass the shell entirely. */
    if (host_gui_test_active()) {
        const char *driver=SDL_GetCurrentVideoDriver();
        if (driver && (!strcmp(driver,"dummy") || !strcmp(driver,"offscreen"))) return 1;
    }
#endif
    host.window = SDL_CreateWindow(HOST_TITLE, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                   960, 720, SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI
#ifdef FF8_VULKAN
                                   | (host_gpu_enabled() ? SDL_WINDOW_VULKAN : 0)
#endif
                                   );
#ifdef FF8_VULKAN
    if (!host.window && host_gpu_enabled()) {
        fprintf(stderr,"Vulkan window unavailable: %s; using soft presentation\n",SDL_GetError());
        host_gpu_stop();
        host.window=SDL_CreateWindow(HOST_TITLE,SDL_WINDOWPOS_CENTERED,SDL_WINDOWPOS_CENTERED,
            960,720,SDL_WINDOW_RESIZABLE|SDL_WINDOW_ALLOW_HIGHDPI);
    }
#endif
    if (!host.window) { fail(SDL_GetError()); return 0; }
#ifdef FF8_VULKAN
    if (host_gpu_enabled()) return 1;
#endif
    return soft_renderer();
}

void host_display_capture(unsigned step, uint64_t first_frame) {
    host.capture_step = step; host.capture_from = first_frame;
}

#ifdef FF8_VULKAN
int host_display_gpu(gpu_t *g) { return host_gpu_start(host.source ? host.source : g,host.window) && soft_fallback(); }
int host_display_scaled_dump(gpu_t *g, const char *path) {
    int w,h;
    const uint8_t *rgb;
    return host_gpu_rgb(g,&rgb,&w,&h) && host_png(path,rgb,w,h);
}
#endif

int host_display_present_capture(const char *dir,unsigned width,unsigned height) {
    host.presented=dir; host.present_w=width; host.present_h=height;
    return !dir || make_directory(dir);
}
static const uint8_t *read_pixels(void *pixels) { return pixels; }
static int headless_renderer(void) {
    if (host.renderer) return 1;
    host.surface=SDL_CreateRGBSurfaceWithFormat(0,(int)host.present_w,(int)host.present_h,32,SDL_PIXELFORMAT_RGBA32);
    if (host.surface) host.renderer=SDL_CreateSoftwareRenderer(host.surface);
    return host.renderer!=NULL;
}
static void soft_screenshot(host_rect_t frame) {
    SDL_Rect rect={frame.x,frame.y,frame.w,frame.h};
    uint8_t *rgb=malloc((size_t)frame.w*frame.h*3);
    if (!rgb) host_screenshot_fail("out of memory");
    else if (SDL_RenderReadPixels(host.renderer,&rect,SDL_PIXELFORMAT_RGB24,rgb,frame.w*3)) {
        free(rgb); host_screenshot_fail("cannot read");
    } else host_screenshot_submit(frame.w,frame.h,HOST_SCREENSHOT_RGB,rgb,read_pixels,free);
}
static int soft_osd(host_viewport_t view,int rw,int rh) {
        {
            const host_osd_panel_t *panel=host_osd_panel(rw,rh,host_osd_area(view));
            if (panel) {
                if (!host.osd_texture || host.osd_revision!=panel->revision) {
                    SDL_DestroyTexture(host.osd_texture);
                    host.osd_texture=SDL_CreateTexture(host.renderer,SDL_PIXELFORMAT_RGBA32,SDL_TEXTUREACCESS_STREAMING,panel->w,panel->h);
                    if (!host.osd_texture || SDL_SetTextureBlendMode(host.osd_texture,SDL_BLENDMODE_BLEND) ||
                        SDL_SetTextureScaleMode(host.osd_texture,SDL_ScaleModeNearest) ||
                        SDL_UpdateTexture(host.osd_texture,NULL,panel->rgba,panel->w*4)) { fail(SDL_GetError()); return 0; }
                    host.osd_revision=panel->revision;
                }
                SDL_Rect rect={panel->x,panel->y,panel->w,panel->h};
                if (SDL_SetTextureAlphaMod(host.osd_texture,panel->opacity) ||
                    SDL_RenderCopy(host.renderer,host.osd_texture,NULL,&rect)) { fail(SDL_GetError()); return 0; }
            }
        }
    return 1;
}
#ifdef FF8_GUI
static void gui_poll(void) {
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        host_gui_test_event(&event);
        if (event.type==SDL_QUIT) { host.quit=1; host_gui_close(); }
        if (host_gui_visible()) {
            host_gui_event(&event);
            if (event.type==SDL_WINDOWEVENT || event.type==SDL_CONTROLLERDEVICEADDED || event.type==SDL_CONTROLLERDEVICEREMOVED)
                input_event(&event);
        } else if (event.type==SDL_KEYDOWN && !event.key.repeat &&
                   (event.key.keysym.scancode==SDL_SCANCODE_ESCAPE || event.key.keysym.sym==SDLK_ESCAPE)) {
            if (host_gui_enabled()) host_gui_request(HOST_GUI_GAME);
            else host.quit=1;
        } else {
            if (host_gui_enabled()) host_gui_event(&event);
            input_event(&event);
        }
        if (event.type==SDL_DROPFILE) SDL_free(event.drop.file);
    }
}
int host_display_gui_attach(void) {
    if (host.gui_attached) return 1;
    if (!host.initialized || SDL_InitSubSystem(SDL_INIT_EVENTS)) return 0;
#ifdef FF8_VULKAN
    if (!host_gpu_enabled())
#endif
    if (!host.renderer && !headless_renderer()) return 0;
    if (host.window && !host_gui_test_finished()) SDL_SetWindowSize(host.window,(int)host.present_w,(int)host.present_h);
    host.gui_attached=gui_ui_attach(host.window,host.renderer);
    return host.gui_attached;
}
void host_display_gui_action(unsigned code) { action(code,1); }
void host_display_gui_audio(void (*pause)(void),void (*resume)(void)) { host.audio_pause=pause; host.audio_resume=resume; }
unsigned host_display_state_slot_value(void) { return host.state_slot; }
unsigned host_display_disc_value(void) { return host.inserted ? host.inserted : 1; }
const char *host_display_gui_device(void) {
#ifdef FF8_VULKAN
    if (host_gpu_enabled()) return host_gpu_device_name();
#endif
    return host_gui_string("val.soft");
}
const char *host_display_gui_controller(void) { return host.controllers[0] ? SDL_GameControllerName(host.controllers[0]) : NULL; }
int host_display_gui_controller_type(void) { return host.controllers[0] ? (int)SDL_GameControllerGetType(host.controllers[0]) : SDL_CONTROLLER_TYPE_UNKNOWN; }
int host_display_gui_fullscreen_exclusive(void) {
    return host.window && (SDL_GetWindowFlags(host.window)&SDL_WINDOW_FULLSCREEN_DESKTOP)==SDL_WINDOW_FULLSCREEN;
}
host_gui_result_t host_display_gui_loop(int shell) {
    uint64_t start;
    host_gui_result_t result;
#ifdef FF8_VULKAN
    /* The menu draws with the renderer here: smooth motion's thread waits. */
    host_display_park(1);
#endif
    host_input_pause(1); clear_pad(); memset(host.keys,0,sizeof(host.keys));
    if (host.audio_pause) host.audio_pause();
    host_osd_pause(1);
    if (!host_display_gui_attach()) {
        fail("cannot initialise GUI");
#ifdef FF8_VULKAN
        host_display_park(0);
#endif
        return HOST_GUI_ERROR;
    }
    gui_ui_reset_input();
    while (host_gui_visible() && !host.quit && host_gui_result()==HOST_GUI_RUNNING) {
        int rw=(int)host.present_w,rh=(int)host.present_h,ok=1;
        const char *shot; uint64_t build_start,build_end,present_end;
        start=SDL_GetPerformanceCounter();
        host_gui_test_tick(host.frame); gui_poll();
        if (host.quit || !host_gui_visible()) break;
        if (host.renderer) SDL_GetRendererOutputSize(host.renderer,&rw,&rh);
        else if (host.window) SDL_GetWindowSize(host.window,&rw,&rh);
        build_start=SDL_GetPerformanceCounter();
        host_gui_frame((unsigned)rw,(unsigned)rh);
        build_end=SDL_GetPerformanceCounter();
        host_gui_test_observe();
        /* A menu action may close during layout. Deferred actions, including
         * screenshot, belong to the next ordinary guest presentation. */
        if (!host_gui_visible()) break;
        shot=host_gui_test_capture();
#ifdef FF8_VULKAN
        if (host_gpu_enabled()) {
            ok=host_gpu_present();
            if (ok && shot) {
                const uint8_t *rgb; int w,h;
                ok=host_gpu_gui_rgb(&rgb,&w,&h) && host_png(shot,rgb,w,h);
                host_gui_test_captured(ok);
            }
            if (ok) ok=host_gpu_gui_frame();
        } else
#endif
        if (host.renderer) {
            host_viewport_t view=host_viewport(host.retained_timing,host.width,host.height,rw,rh,host.scale_mode);
            SDL_SetRenderDrawColor(host.renderer,0,0,0,255);
            SDL_RenderSetClipRect(host.renderer,NULL);
            ok=SDL_RenderClear(host.renderer)==0;
            if (ok && !shell && host.texture) {
                SDL_Rect dest={view.content.x,view.content.y,view.content.w,view.content.h};
                SDL_Rect clip={view.frame.x,view.frame.y,view.frame.w,view.frame.h};
                SDL_RenderSetClipRect(host.renderer,&clip);
                ok=SDL_RenderCopy(host.renderer,host.texture,NULL,&dest)==0;
                SDL_RenderSetClipRect(host.renderer,NULL);
            }
            gui_ui_soft_render(host.renderer);
            if (ok) ok=soft_osd(view,rw,rh);
            if (shot) {
                uint8_t *rgb=malloc((size_t)rw*rh*3);
                int captured=rgb && !SDL_RenderReadPixels(host.renderer,NULL,SDL_PIXELFORMAT_RGB24,rgb,rw*3) && host_png(shot,rgb,rw,rh);
                free(rgb); host_gui_test_captured(captured);
            }
            SDL_RenderPresent(host.renderer);
        }
#ifdef FF8_VULKAN
        if (host_gpu_enabled()) { double ms; while ((ms=host_gpu_gui_timing())>=0) host_gui_test_gpu_timing(ms); }
#endif
        present_end=SDL_GetPerformanceCounter();
        host_gui_test_timing((build_end-build_start)*1000.0/SDL_GetPerformanceFrequency(),
                             (present_end-build_end)*1000.0/SDL_GetPerformanceFrequency(),
                             (present_end-start)*1000.0/SDL_GetPerformanceFrequency());
        if (!ok || host_gui_test_failed()) { fail("GUI presentation or script failed"); break; }
        {
            double ms=(SDL_GetPerformanceCounter()-start)*1000.0/SDL_GetPerformanceFrequency();
            if (ms<1000.0/60) SDL_Delay((Uint32)(1000.0/60-ms));
        }
    }
    gui_ui_reset_input();
    clear_pad(); memset(host.keys,0,sizeof(host.keys)); host_osd_clear(HOST_OSD_FAST);
    host_osd_pause(0);
    if (host.audio_resume) host.audio_resume();
    host_input_pause(0);
    result = host.failed ? HOST_GUI_ERROR : host.quit ? HOST_GUI_QUIT : host_gui_result();
#ifdef FF8_VULKAN
    host_display_park(0);
#endif
    return result;
}
#endif
#ifdef FF8_VULKAN
/* Smooth motion: pictures on the display's own clock, not the guest's
 * (present_pace.h). Each picture is for one refresh of the display, every
 * refresh or every few as its cost allows, and shows the game at the moment it
 * is begun, kept at the same distance before that refresh, so that pictures
 * evenly spaced on the display show the game evenly spaced. Where the device
 * says when each picture was shown (present timing), pictures follow the
 * display's refreshes and are held until their own, so that when they are
 * handed over does not matter as long as it is before the refresh before.
 * Elsewhere they are handed over just before that refresh, on the times each
 * present was seen. The picture is drawn on a thread of its own, so neither
 * the game thread's other work nor the time a picture takes moves it. */
enum { SMOOTH_PENDING = 32 };
/* The thread that draws and presents while smooth motion runs in a window: the
 * guest thread then only queues what it sends its GPU. The game thread parks
 * it (host_display_park) to use the renderer itself: the menu, shutting down. */
static struct {
    SDL_Thread *thread;
    SDL_mutex *running, *clock;
    SDL_atomic_t stop;
    void (*idle)(void *context);
} presenter;
static struct {
    host_display_draw_t draw;
    void *context;
    gpu_t *shown;
    int rate, timing, have, whole;
    double at, period, frequency, start;   /* the guest's last VBlank (smoothed) and its period, seconds */
    double vblank;                          /* when the guest's last VBlank came */
    double refresh;                         /* the display's refresh period as last found */
    present_pace_t pace;
    double left, work;                      /* when the guest's work began, how long it takes */
    uint64_t ids[SMOOTH_PENDING];
    double moments[SMOOTH_PENDING], targets[SMOOTH_PENDING], begun[SMOOTH_PENDING], presented[SMOOTH_PENDING];
    double since[SMOOTH_PENDING], vblanks[SMOOTH_PENDING];
    unsigned multiples[SMOOTH_PENDING];
    int repeats[SMOOTH_PENDING];
    unsigned pending, first;
    uint64_t presents;
    double spent[4];                        /* seconds drawing, handing over, waiting to hand over, holding the guest */
    double wake, checked;                   /* when the next picture is begun; the display last checked */
    FILE *stats;
} smooth;
static double seconds(void) { return (double)SDL_GetPerformanceCounter() / smooth.frequency; }
/* The display the window is on, checked again now and then: the window may be
 * moved to another display, and what the device can say of the window's
 * display is known only once it has presented to it. */
static void smooth_display(void) {
    SDL_DisplayMode mode;
    int index = host.window ? SDL_GetWindowDisplayIndex(host.window) : -1;
    int rate = index >= 0 && !SDL_GetCurrentDisplayMode(index, &mode) && mode.refresh_rate > 0 ? mode.refresh_rate : 60;
    int timing = host_gpu_present_timing();
    double reported = timing ? host_gpu_refresh() : 0;
    /* Where the device does not say the period (a virtual output), the pacer
     * finds it from when pictures were shown, starting from the mode's. */
    double refresh = reported > 0 ? reported : 1.0 / rate;
    if (smooth.pace.refresh > 0 && rate == smooth.rate && timing == smooth.timing && (reported > 0) == smooth.pace.known &&
        fabs(refresh - smooth.refresh) < 1e-7) return;
    smooth.rate = rate; smooth.timing = timing; smooth.refresh = refresh;
    present_pace_start(&smooth.pace, refresh, reported > 0, timing > 0, timing > 1);
}
static void smooth_totals(const char *when) {
    const present_pace_t *p = &smooth.pace;
    fprintf(stderr, "Smooth motion%s: pictures=%" PRIu64 " on-time=%lu late=%lu early=%lu stray=%lu skipped=%lu changes=%lu"
            " multiple=%u lead=%.1f ms drawing=%.1f s handing-over=%.1f s waiting=%.1f s holding=%.1f s\n",
            when, smooth.presents, p->on_time, p->late, p->early, p->stray, p->skipped, p->changes, p->multiple,
            p->lead * 1000, smooth.spent[0], smooth.spent[1], smooth.spent[2], smooth.spent[3]);
}
void host_display_smooth(host_display_draw_t draw, void *context) {
    if (!draw) {
        FILE *stats = smooth.stats;
        if (smooth.draw && smooth.presents) smooth_totals("");
        memset(&smooth, 0, sizeof(smooth)); smooth.stats = stats; host_gpu_paced(0); return;
    }
    host_gpu_paced(1);
    smooth.draw = draw; smooth.context = context;
    if (!smooth.frequency) {
        smooth.frequency = (double)SDL_GetPerformanceFrequency();
        smooth.start = seconds();
        smooth_display();
    }
}
int host_display_present_stats(const char *path) {
    if (!path) return 1;
    if (!(smooth.stats = fopen(path, "w"))) return 0;
    return fprintf(smooth.stats, "picture,moment_ms,begun_ms,presented_ms,shown_ms,refreshes_late,stray,since,refresh_ms,"
                   "multiple,target_ms,repeat,vblank_ms\n") > 0;
}
uint64_t host_display_pictures(void) { return smooth.presents; }
int host_display_smoothing(void) { return smooth.draw && host.window && host_gpu_enabled() && !host.quit; }
void host_display_smooth_vblank(int pal, int whole) {
    double now = seconds(), period = pal ? 1.0 / 50 : 1001.0 / 60000;
    if (presenter.clock) SDL_LockMutex(presenter.clock);
    if (smooth.left > 0) {
        double work = now - smooth.left;
        smooth.work = work > smooth.work ? work : smooth.work + 0.05 * (work - smooth.work);
    }
    /* The guest's VBlanks come when pacing lets them, a millisecond early or
     * late; the pictures follow its average clock, not each wake-up. */
    if (!smooth.have || now - (smooth.at + period) > 2 * period || now - (smooth.at + period) < -2 * period) smooth.at = now;
    else smooth.at += period + 0.05 * (now - (smooth.at + period));
    smooth.period = period; smooth.have = 1; smooth.whole = whole; smooth.vblank = now;
    if (presenter.clock) SDL_UnlockMutex(presenter.clock);
}
static double ms(double t) { return (t - smooth.start) * 1000; }
static void stats_line(unsigned i, const double *shown, int late, int stray) {
    if (!smooth.stats) return;
    fprintf(smooth.stats, "%" PRIu64 ",%.3f,%.3f,%.3f,", smooth.ids[i], ms(smooth.moments[i]), ms(smooth.begun[i]), ms(smooth.presented[i]));
    if (shown) fprintf(smooth.stats, "%.3f,%d,%d,", ms(*shown), late, stray);
    else fprintf(smooth.stats, ",,,");
    fprintf(smooth.stats, "%.4f,%.4f,%u,", smooth.since[i], smooth.pace.refresh * 1000, smooth.multiples[i]);
    if (smooth.targets[i] > 0) fprintf(smooth.stats, "%.3f", ms(smooth.targets[i]));
    fprintf(smooth.stats, ",%d,%.3f\n", smooth.repeats[i], ms(smooth.vblanks[i]));
}
/* What the display said of the pictures before. Presents it never reports
 * (the swapchain was made again) are written without. */
static void smooth_feedback(void) {
    uint64_t seen;
    double when;
    while (host_gpu_present_seen(&seen, &when)) {
        while (smooth.pending) {
            unsigned i = smooth.first % SMOOTH_PENDING;
            if (smooth.ids[i] > seen) break;
            if (smooth.ids[i] == seen) {
                int stray, late = present_pace_shown(&smooth.pace, smooth.moments[i], when, smooth.repeats[i], &stray);
                stats_line(i, &when, late, stray);
            } else stats_line(i, NULL, 0, 0);
            ++smooth.first; --smooth.pending;
        }
    }
}
/* A present handed over, until the display reports it. */
static void pending(const present_plan_t *plan, double moment, double target, double begun, double since, double vblank, int repeat) {
    uint64_t id = host_gpu_present_id();
    unsigned i;
    if (smooth.pending == SMOOTH_PENDING) { stats_line(smooth.first % SMOOTH_PENDING, NULL, 0, 0); ++smooth.first; --smooth.pending; }
    i = (smooth.first + smooth.pending++) % SMOOTH_PENDING;
    smooth.ids[i] = id ? id : smooth.presents + 1; smooth.moments[i] = moment; smooth.targets[i] = target;
    smooth.begun[i] = begun; smooth.presented[i] = seconds(); smooth.since[i] = since; smooth.multiples[i] = plan->multiple;
    smooth.repeats[i] = repeat; smooth.vblanks[i] = vblank;
    /* Without present ids nothing will report it. */
    if (!id) { stats_line(i, NULL, 0, 0); --smooth.pending; }
}
/* The guest's next VBlank is due in ms: if the next picture would be due
 * while the guest works on it, it is drawn first, and the guest begins that
 * much later; its own clock and the audio do not change. */
void host_display_hold(double ms_left) {
    double now, start;
    present_plan_t plan;
    if (!host_display_smoothing() || smooth.pace.refresh <= 0) return;
    now = seconds();
    smooth.left = now + ms_left / 1000;
    if (ms_left > 1.5 || !smooth.presents) return;
    present_pace_plan(&smooth.pace, now, &plan);
    start = plan.begin;
    if (start > smooth.left + smooth.work || start < now - 0.25 * smooth.pace.refresh) return;
    {
        double from = seconds();
        while (seconds() < start) SDL_Delay(start - seconds() > 0.0015 ? 1 : 0);
        smooth.spent[3] += seconds() - from;
    }
    host_display_tick();
    smooth.left = seconds();
}
int host_display_tick(void) {
    double now, since, at, period, begun, drawn, waited = 0, handed, cost, vblank;
    int have, whole;
    present_plan_t plan;
    unsigned r;
    if (!host_display_smoothing()) return 0;
    now = seconds();
    smooth.wake = now + 0.002;
#ifdef FF8_GUI
    if (host_gui_visible()) return 0;
#endif
    if (now - smooth.checked > 1) { smooth.checked = now; smooth_display(); }
    smooth_feedback();
    present_pace_plan(&smooth.pace, now, &plan);
    smooth.wake = plan.begin;
    if (now < plan.begin) return 0;
    if (host_gpu_ready() <= 0) return 0;
    if (presenter.clock) SDL_LockMutex(presenter.clock);
    at = smooth.at; period = smooth.period; have = smooth.have; whole = smooth.whole; vblank = smooth.vblank;
    if (presenter.clock) SDL_UnlockMutex(presenter.clock);
    since = have ? (plan.guest - at) / period : 0;
    if (since < 0) since = 0;
    if (since > 1) since = 1;
    begun = seconds();
    smooth.shown = smooth.draw(smooth.context, since, whole);
    if (!host_gpu_scanout(smooth.shown)) return 0;
    {
        int ok;
        host_osd_hold(1);
        ok = host_gpu_present();
        host_osd_hold(0);
        if (!ok) return 0;
    }
    drawn = seconds();
    smooth.spent[0] += drawn - begun;
    if (host_gpu_presented_frames()) {
        const uint8_t *rgb; int w, h;
        size_t size = strlen(host_gpu_presented_frames()) + 40;
        char *path = malloc(size);
        int ok = 0;
        if (path && host_gpu_presented_rgb(&rgb, &w, &h)) {
            snprintf(path, size, "%s/frame-%06llu.png", host_gpu_presented_frames(), (unsigned long long)smooth.presents);
            ok = host_png(path, rgb, w, h);
        }
        free(path);
        if (!ok) { fail("cannot write presented PNG"); return 0; }
        waited += seconds() - drawn;
    }
    /* Held until its refresh where the device can; elsewhere handed over at
     * its time. */
    if (plan.target > 0) host_gpu_present_at(plan.target);
    else if (plan.handover > 0) {
        double from = seconds();
        while (seconds() < plan.handover) SDL_Delay(plan.handover - seconds() > 0.0015 ? 1 : 0);
        smooth.spent[2] += seconds() - from;
        waited += seconds() - from;
    }
    handed = seconds();
    host_gpu_frame(smooth.shown);
    smooth.spent[1] += seconds() - handed;
    pending(&plan, plan.moment, plan.target, begun, since, vblank, 0);
    cost = seconds() - begun - waited;
    /* The same picture again for each refresh until the next, held until its
     * own, so that the compositor always has something for the next refresh. */
    for (r = 1; r <= plan.repeats && host_gpu_ready() > 0; ++r) {
        double moment = plan.moment + r * smooth.pace.refresh, from = seconds();
        if (!host_gpu_present()) break;
        host_gpu_present_at(plan.target + r * smooth.pace.refresh);
        host_gpu_frame(smooth.shown);
        smooth.spent[1] += seconds() - from;
        pending(&plan, moment, plan.target + r * smooth.pace.refresh, from, since, vblank, 1);
    }
    present_pace_done(&smooth.pace, &plan, cost, r > 1 ? (seconds() - begun - waited - cost) / (r - 1) : 0, seconds());
    ++smooth.presents;
    return 1;
}
static int presenter_main(void *unused) {
    (void)unused;
    while (!SDL_AtomicGet(&presenter.stop)) {
        double wait;
        SDL_LockMutex(presenter.running);
        /* A picture due goes first; what the guest queued is taken up in the
         * time left before the next (drawing takes it up anyway). */
        host_display_tick();
        if (presenter.idle && smooth.wake - seconds() > 0.002) presenter.idle(smooth.context);
        wait = smooth.wake - seconds();
        SDL_UnlockMutex(presenter.running);
        /* Asleep until a millisecond and a half before it, the rest awake. */
        SDL_Delay(wait > 0.0025 ? (Uint32)((wait - 0.0015) * 1000) : 0);
    }
    return 0;
}
int host_display_presenter(void (*idle)(void *context), int on) {
    if (on && !presenter.thread) {
        if (!presenter.running && !(presenter.running = SDL_CreateMutex())) return 0;
        if (!presenter.clock && !(presenter.clock = SDL_CreateMutex())) return 0;
        presenter.idle = idle;
        SDL_AtomicSet(&presenter.stop, 0);
        if (!(presenter.thread = SDL_CreateThread(presenter_main, "lionheart-present", NULL))) return 0;
    } else if (!on && presenter.thread) {
        SDL_AtomicSet(&presenter.stop, 1);
        SDL_WaitThread(presenter.thread, NULL);
        presenter.thread = NULL;
    }
    return 1;
}
int host_display_presenting(void) { return presenter.thread != NULL; }
void host_display_park(int park) {
    if (!presenter.thread) return;
    if (park) SDL_LockMutex(presenter.running); else SDL_UnlockMutex(presenter.running);
}
#endif
static void present(gpu_t *g, int capture) {
    int w, h;
    if (!host.initialized || host.quit) return;
    host_osd_frame(host.window!=NULL,host.osd_frame);
    if (capture) { host_screenshot_frame(host.osd_frame); ++host.osd_frame; }
#ifdef FF8_GUI
    if (host_gui_enabled()) {
        host_gui_test_tick(host.frame); 
        /* Toasts are drawn with the menu's state, by smooth motion's thread too. */
        host_osd_hold(1); gui_poll(); host_osd_hold(0);
        if (host_gui_requested()) host_gui_open();
    } else
#endif
    if (host.video) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            input_event(&event);
            if (event.type == SDL_QUIT ||
                (event.type == SDL_KEYDOWN && event.key.keysym.sym == SDLK_ESCAPE)) host.quit = 1;
        }
    }
    if (host.quit) { host_input_pause(1); return; }
#ifdef FF8_VULKAN
    if (host.crt && !host_gpu_enabled() && !host.crt_warned) {
#else
    if (host.crt && !host.crt_warned) {
#endif
        host.crt_warned = 1;
        fprintf(stderr, "CRT preset %s needs the Vulkan renderer; presenting normally\n", host_crt_names[host.crt]);
    }
#ifdef FF8_VULKAN
    /* Smooth motion presents on the display's clock (host_display_tick). */
    if (host_display_smoothing()) { if (capture) ++host.frame; return; }
    if (host_gpu_enabled()) {
        const char *scaled=host_gpu_scaled_frames(), *presented=host_gpu_presented_frames();
        int take_capture=capture && host.frame>=host.capture_from && (!host.capture_step || host.frame%host.capture_step==0);
        if (scaled && take_capture) {
            const uint8_t *rgb;
            if (!host_gpu_rgb(g,&rgb,&w,&h)) goto gpu_fallback;
            if (scaled && take_capture) {
                size_t size=strlen(scaled)+40;
                char *path=malloc(size);
                int ok=0;
                if (path) { snprintf(path,size,"%s/frame-%06llu.png",scaled,host.frame); ok=host_png(path,rgb,w,h); free(path); }
                if (!ok) { fail("cannot write scaled PNG"); return; }
            }
        }
        if (host.video || presented || host_screenshot_pending()
#ifdef FF8_GUI
            || host_gui_test_capture()
#endif
            ) {
            if (!host_gpu_scanout(g) || !host_gpu_present()) goto gpu_fallback;
        }
#ifdef FF8_GUI
        if (host_gui_test_capture()) {
            const uint8_t *rgb; int cw,ch;
            int ok=host_gpu_gui_rgb(&rgb,&cw,&ch) && host_png(host_gui_test_capture(),rgb,cw,ch);
            host_gui_test_captured(ok);
        }
#endif
#ifdef FF8_GUI
        /* Diagnostic overlays still need the preceding guest image when no WSI
         * or ordinary presented dump is active. Composition waits for the menu. */
        else if (host_gui_test_active() && !host_gpu_scanout(g)) goto gpu_fallback;
#endif
        if (presented && take_capture) {
            const uint8_t *rgb;
            size_t size=strlen(presented)+40;
            char *path=malloc(size);
            int ok=0;
            if (!host_gpu_presented_rgb(&rgb,&w,&h)) { free(path); goto gpu_fallback; }
            if (path) { snprintf(path,size,"%s/frame-%06llu.png",presented,host.frame); ok=host_png(path,rgb,w,h); free(path); }
            if (!ok) { fail("cannot write presented PNG"); return; }
        }
        if (!host_gpu_frame(g)) goto gpu_fallback;
    }
gpu_fallback:
    if (!soft_fallback()) return;
#endif
    if (!host.renderer && (host.presented || host_screenshot_pending())
#ifdef FF8_VULKAN
        && !host_gpu_enabled()
#endif
        && !headless_renderer()) { host_screenshot_fail("cannot present"); return; }
    if (!host.renderer && !host.frames) { if (capture) ++host.frame; return; }
    if (!host_display_rgb(g, host.rgb, sizeof(host.rgb), &w, &h)) { fail("invalid display dimensions"); return; }
    if (!w || !h) { w = h = 1; memset(host.rgb, 0, 3); }
    if (capture && host.frames && host.frame >= host.capture_from &&
        (!host.capture_step || host.frame % host.capture_step == 0)) {
        size_t size = strlen(host.frames) + 40;
        char *path = malloc(size);
        int ok = 0;
        if (path) {
            snprintf(path, size, "%s/frame-%06llu.png", host.frames, host.frame);
            ok = host_png(path, host.rgb, w, h); free(path);
        }
        if (!ok) { fail("cannot write frame PNG"); return; }
    }
    if (host.renderer) {
        int rw, rh;
        SDL_Rect dest, clip;
        if (!host.texture || w != host.width || h != host.height) {
            SDL_DestroyTexture(host.texture);
            host.texture = SDL_CreateTexture(host.renderer, SDL_PIXELFORMAT_RGB24, SDL_TEXTUREACCESS_STREAMING, w, h);
            host.width = w; host.height = h;
            if (!host.texture) { fail(SDL_GetError()); return; }
            if (SDL_SetTextureScaleMode(host.texture, host.linear ? SDL_ScaleModeLinear : SDL_ScaleModeNearest)) {
                fail(SDL_GetError()); return;
            }
        }
        if (SDL_UpdateTexture(host.texture, NULL, host.rgb, w * 3) || SDL_GetRendererOutputSize(host.renderer, &rw, &rh)) {
            fail(SDL_GetError()); return;
        }
        #ifdef FF8_GUI
        host.retained_timing=gpu_scanout(g);
#endif
        host_viewport_t view = host_viewport(gpu_scanout(g), w, h, rw, rh, host.scale_mode);
        dest.x = view.content.x; dest.y = view.content.y; dest.w = view.content.w; dest.h = view.content.h;
        clip.x = view.frame.x; clip.y = view.frame.y; clip.w = view.frame.w; clip.h = view.frame.h;
        SDL_SetRenderDrawColor(host.renderer, 0, 0, 0, 255);
        if (SDL_RenderClear(host.renderer) || SDL_RenderSetClipRect(host.renderer, &clip) ||
            (dest.w > 0 && dest.h > 0 && SDL_RenderCopy(host.renderer, host.texture, NULL, &dest)) ||
            SDL_RenderSetClipRect(host.renderer, NULL)) {
            fail(SDL_GetError()); return;
        }
        if (host_screenshot_pending()) soft_screenshot(view.frame);
#ifdef FF8_GUI
        if (host_gui_toast_visible() && host_display_gui_attach()) {
            host_gui_toast_frame((unsigned)rw,(unsigned)rh,host_osd_area(view));
            gui_ui_soft_render(host.renderer);
        }
#endif
        if (!soft_osd(view,rw,rh)) return;
#ifdef FF8_GUI
        if (host_gui_test_capture()) {
            uint8_t *rgb=malloc((size_t)rw*rh*3);
            int ok=rgb && !SDL_RenderReadPixels(host.renderer,NULL,SDL_PIXELFORMAT_RGB24,rgb,rw*3) && host_png(host_gui_test_capture(),rgb,rw,rh);
            free(rgb); host_gui_test_captured(ok);
        }
#endif
        if (capture && host.presented && host.frame>=host.capture_from && (!host.capture_step || host.frame%host.capture_step==0)) {
            uint8_t *rgb=malloc((size_t)rw*rh*3);
            size_t n=strlen(host.presented)+40; char *path=malloc(n);
            int ok=0;
            if (rgb && path && !SDL_RenderReadPixels(host.renderer,NULL,SDL_PIXELFORMAT_RGB24,rgb,rw*3)) {
                snprintf(path,n,"%s/frame-%06llu.png",host.presented,host.frame); ok=host_png(path,rgb,rw,rh);
            }
            free(rgb); free(path);
            if (!ok) { fail("cannot write presented PNG"); return; }
        }
        SDL_RenderPresent(host.renderer);
    }
    if (capture) ++host.frame;
}

void host_present(gpu_t *g) { present(g,1); }
void host_display_source(gpu_t *g) { host.source = g; }
void host_display_rebind(gpu_t *g) {
#ifdef FF8_VULKAN
    if (host_gpu_enabled()) host_gpu_resume(g);
#else
    (void)g;
#endif
}
void host_display_recover(gpu_t *g, uint64_t vblank) {
    if (host.source) g = host.source;
#ifdef FF8_VULKAN
    /* Smooth motion's thread starts its renderer over itself, as it takes up
     * the restored state. */
    if (host_gpu_enabled() && !host_display_presenting()) host_gpu_resume(g);
#endif
    host_osd_clear(HOST_OSD_FAST);
    memset(host.keys,0,sizeof(host.keys));
    clear_pad();
    host.disc_request = host.state_action = 0;
    SDL_DestroyTexture(host.texture); host.texture = NULL;
    host.frame = vblank; present(g,0);
}
int host_display_running(void) { return !host.quit; }
int host_display_failed(void) { return host.failed; }

int host_display_shutdown(gpu_t *g) {
    int ok;
    unsigned port;
#ifdef FF8_VULKAN
    host_display_presenter(NULL, 0);
    if (smooth.presents) smooth_totals("");
    /* Pictures the display never reported, as the window closed. */
    while (smooth.pending) { stats_line(smooth.first++ % SMOOTH_PENDING, NULL, 0, 0); --smooth.pending; }
    if (smooth.stats && fclose(smooth.stats)) host.failed = 1;
    memset(&smooth, 0, sizeof(smooth));
#endif
    host_input_pause(1);
    if (host.vram_path && (!g || !host_vram_dump(g, host.vram_path))) fail("cannot write VRAM PNG");
    host_screenshot_shutdown();
#ifdef FF8_GUI
    if (host.gui_attached) gui_ui_detach();
#endif
#ifdef FF8_VULKAN
    host_gpu_stop();
#endif
    host_osd_shutdown();
    SDL_DestroyTexture(host.osd_texture);
    SDL_DestroyTexture(host.texture); SDL_DestroyRenderer(host.renderer); SDL_FreeSurface(host.surface); SDL_DestroyWindow(host.window);
    for (port = 0; port < 2; ++port) if (host.controllers[port]) SDL_GameControllerClose(host.controllers[port]);
    if (host.video) SDL_QuitSubSystem(SDL_INIT_GAMECONTROLLER);
    if (host.video) SDL_QuitSubSystem(SDL_INIT_VIDEO);
    ok = !host.failed;
    memset(&host, 0, sizeof(host));
    return ok;
}
