#include "display.h"
#include "input.h"
#include "hw.h"
#include <SDL.h>
#include <assert.h>
#include <stdlib.h>
#include <string.h>

static cpu_t *c;
static gpu_t *g;
static host_input_t input = {.dualshock = 1, .rumble = 1};
static struct { Uint16 low, high; Uint8 led; unsigned calls, led_calls; } feedback[2];
static SDL_JoystickID ids[2] = {-1, -1};
int __real_SDL_GameControllerRumble(SDL_GameController *, Uint16, Uint16, Uint32);
int __real_SDL_GameControllerSetLED(SDL_GameController *, Uint8, Uint8, Uint8);
int __wrap_SDL_GameControllerSetLED(SDL_GameController *controller, Uint8 r, Uint8 green, Uint8 blue) {
    SDL_JoystickID id = SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(controller));
    for (unsigned p = 0; p < 2; ++p) if (ids[p] == id) ++feedback[p].led_calls;
    return __real_SDL_GameControllerSetLED(controller, r, green, blue);
}
int __wrap_SDL_GameControllerRumble(SDL_GameController *controller, Uint16 low, Uint16 high, Uint32 ms) {
    SDL_JoystickID id = SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(controller));
    for (unsigned p = 0; p < 2; ++p) if (ids[p] == id) {
        feedback[p].low = low; feedback[p].high = high; ++feedback[p].calls;
    }
    return __real_SDL_GameControllerRumble(controller, low, high, ms);
}
static int rumble(void *port, Uint16 low, Uint16 high) {
    (void)port; (void)low; (void)high;
    return 0;
}
static int led(void *port, Uint8 r, Uint8 green, Uint8 blue) {
    unsigned p = (unsigned)(uintptr_t)port;
    assert(!green && !blue); feedback[p].led = r; return 0;
}
static void pump(void) {
    host_present(g); host_input_sync(c, &input);
    /* SDL virtual devices deliver feedback callbacks on their next update. */
    SDL_JoystickUpdate();
}
static void event(SDL_Event *e) { assert(SDL_PushEvent(e) == 1); pump(); }
static void window(unsigned kind) {
    SDL_Event e; SDL_zero(e); e.type = SDL_WINDOWEVENT; e.window.event = (Uint8)kind; event(&e);
}
static void axis(SDL_JoystickID id, unsigned which, Sint16 value) {
    SDL_Event e; SDL_zero(e); e.type = SDL_CONTROLLERAXISMOTION;
    e.caxis.which = id; e.caxis.axis = (Uint8)which; e.caxis.value = value; event(&e);
}
static unsigned wire(unsigned value) {
    hw_write(c, 0x1f801040, 1, value); c->cycles += 2000; hw_event(c);
    return hw_read(c, 0x1f801040, 1);
}
static unsigned packet(unsigned port, unsigned cmd, const uint8_t tx[6], uint8_t rx[9]) {
    hw_write(c, 0x1f80104a, 2, 0);
    hw_write(c, 0x1f801048, 2, 0xd); hw_write(c, 0x1f80104e, 2, 136);
    hw_write(c, 0x1f80104a, 2, 3 | (port << 13));
    rx[0] = (uint8_t)wire(1); rx[1] = (uint8_t)wire(cmd); rx[2] = (uint8_t)wire(0);
    unsigned n = (rx[1] & 15) * 2;
    assert(rx[0] == 0xff && rx[2] == 0x5a && n <= 6);
    for (unsigned i = 0; i < n; ++i) rx[i + 3] = (uint8_t)wire(tx[i]);
    return n + 3;
}
static void configure(unsigned port) {
    uint8_t rx[9], tx[6] = {1, 0, 0, 0, 0, 0};
    packet(port, 0x43, tx, rx); assert(rx[1] == 0x41);
    tx[1] = 3; packet(port, 0x44, tx, rx); assert(rx[1] == 0xf3);
    packet(port, 0x45, tx, rx); assert(rx[5] == 1);
    const uint8_t map[6] = {0, 1, 0xff, 0xff, 0xff, 0xff};
    packet(port, 0x4d, map, rx);
    memset(tx, 0, sizeof(tx)); packet(port, 0x43, tx, rx);
}
int main(int argc, char **argv) {
    c = malloc(sizeof(*c)); assert(c); cpu_init(c); sio_init(c);
    g = gpu_create(); assert(g); assert(host_display_init(0, NULL, NULL)); host_input_dualshock(1);
    /* Selecting DualShock keeps keyboard-only input on a digital pad. */
    {
        SDL_Event e; SDL_zero(e); e.type = SDL_KEYDOWN; e.key.keysym.scancode = SDL_SCANCODE_UP; event(&e);
        uint8_t rx[9], tx[6] = {0};
        assert(packet(0, 0x42, tx, rx) == 5 && rx[1] == 0x41 && rx[3] == 0xef);
        e.type = SDL_KEYUP; event(&e); assert(!host_input_buttons(0));
    }
    int devices[2];
    for (unsigned p = 0; p < 2; ++p) {
        SDL_VirtualJoystickDesc d; SDL_zero(d);
        d.version = SDL_VIRTUAL_JOYSTICK_DESC_VERSION; d.type = SDL_JOYSTICK_TYPE_GAMECONTROLLER;
        d.naxes = 6; d.nbuttons = 16; d.Rumble = rumble; d.SetLED = led; d.userdata = (void *)(uintptr_t)p;
        devices[p] = SDL_JoystickAttachVirtualEx(&d); assert(devices[p] >= 0); pump();
        ids[p] = SDL_JoystickGetDeviceInstanceID(devices[p]); configure(p);
    }
    window(SDL_WINDOWEVENT_FOCUS_GAINED);
    /* D-pad and stick bytes are independent in a real analog-mode reply. */
    for (unsigned p = 0; p < 2; ++p) {
        for (unsigned a = 0; a < 4; ++a) axis(ids[p], a, 0);
        const unsigned directions[] = {SDL_CONTROLLER_BUTTON_DPAD_UP, SDL_CONTROLLER_BUTTON_DPAD_RIGHT,
                                       SDL_CONTROLLER_BUTTON_DPAD_DOWN, SDL_CONTROLLER_BUTTON_DPAD_LEFT};
        for (unsigned d = 0; d < 4; ++d) {
            SDL_Event e; SDL_zero(e); e.type = SDL_CONTROLLERBUTTONDOWN;
            e.cbutton.which = ids[p]; e.cbutton.button = (Uint8)directions[d]; event(&e);
            uint8_t rx[9], tx[6] = {0};
            assert(packet(p, 0x42, tx, rx) == 9);
            const uint8_t expected[] = {0xff, 0x73, 0x5a, (uint8_t)~(1u << (d + 4)), 0xff,
                                       0x80, 0x80, 0x80, 0x80};
            assert(!memcmp(rx, expected, sizeof(expected)));
            e.type = SDL_CONTROLLERBUTTONUP; event(&e);
            packet(p, 0x42, tx, rx); assert(rx[3] == 0xff && rx[4] == 0xff);
        }
    }
    axis(ids[0], SDL_CONTROLLER_AXIS_LEFTX, -32768); axis(ids[0], SDL_CONTROLLER_AXIS_LEFTY, 32767);
    axis(ids[0], SDL_CONTROLLER_AXIS_RIGHTX, -1); axis(ids[0], SDL_CONTROLLER_AXIS_RIGHTY, 0);
    assert(!host_input_buttons(0));
    uint8_t rx[9], motors[6] = {1, 173, 0, 0, 0, 0};
    assert(packet(0, 0x42, motors, rx) == 9 && rx[1] == 0x73);
    assert(rx[3] == 255 && rx[4] == 255);
    assert(rx[5] == 127 && rx[6] == 128 && rx[7] == 0 && rx[8] == 255);
    pump(); assert(feedback[0].low == 173 * 257 && feedback[0].high == 65535 && feedback[0].led == 255);
    assert(!feedback[1].low && !feedback[1].high && feedback[1].led == 255);
    unsigned led_calls = feedback[0].led_calls;
    host_input_dualshock(0); pump();
    assert(feedback[0].led_calls == led_calls);
    host_input_dualshock(1); pump();
    unsigned refreshes = feedback[0].calls;
    pump(); assert(feedback[0].calls > refreshes);
    const uint8_t stopped[6] = {0};
    packet(0, 0x42, stopped, rx); pump(); assert(!feedback[0].low && !feedback[0].high);
    packet(0, 0x42, motors, rx); pump(); assert(feedback[0].low && feedback[0].high);
    input.rumble = 0; pump(); assert(!feedback[0].low && !feedback[0].high);
    input.rumble = 1; pump(); assert(feedback[0].low);
    host_input_pause(1); SDL_JoystickUpdate(); assert(!feedback[0].low && !feedback[0].high);
    host_input_pause(0); pump(); assert(feedback[0].low);
    window(SDL_WINDOWEVENT_FOCUS_LOST); assert(!feedback[0].low && !feedback[0].high);
    packet(0, 0x42, motors, rx); for (unsigned i = 5; i < 9; ++i) assert(rx[i] == 128);
    window(SDL_WINDOWEVENT_FOCUS_GAINED); assert(feedback[0].low);
    /* A script must expose a digital pad even with a live DualShock present. */
    if (argc == 2) {
        char error[256]; assert(sio_load_script(c, argv[1], error, sizeof(error)));
        input.scripted = 1; pump();
        assert(packet(0, 0x42, motors, rx) == 5 && rx[1] == 0x41);
        assert(rx[3] == 255 && rx[4] == 0xbf);
        assert(!feedback[0].low && !feedback[0].high);
        assert(packet(1, 0x42, motors, rx) == 9 && rx[1] == 0x73);
    }
    assert(SDL_JoystickDetachVirtual(devices[0]) == 0); pump();
    assert(!host_input_controller(0) && !host_input_buttons(0));
    assert(packet(0, 0x42, motors, rx) == 5 && rx[1] == 0x41);
    packet(1, 0x42, motors, rx); pump(); assert(feedback[1].low);
    SDL_Event e; SDL_zero(e); e.type = SDL_QUIT; event(&e);
    assert(!feedback[1].low && !feedback[1].high && !host_display_running());
    assert(host_display_shutdown(g)); sio_destroy(c); hw_destroy(c); free(c); free(g);
    return 0;
}
