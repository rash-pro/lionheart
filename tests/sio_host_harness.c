#include "display.h"
#include "sio.h"
#include <SDL.h>
#include <assert.h>
#include <stdlib.h>
#include <string.h>

static gpu_t *g;
static void key(SDL_Scancode code, int down) {
    SDL_Event e; memset(&e, 0, sizeof(e));
    e.type = down ? SDL_KEYDOWN : SDL_KEYUP; e.key.keysym.scancode = code;
    assert(SDL_PushEvent(&e) == 1); host_present(g);
}
static void button(SDL_JoystickID id, unsigned which, int down) {
    SDL_Event e; memset(&e, 0, sizeof(e));
    e.type = down ? SDL_CONTROLLERBUTTONDOWN : SDL_CONTROLLERBUTTONUP;
    e.cbutton.which = id; e.cbutton.button = (Uint8)which;
    assert(SDL_PushEvent(&e) == 1); host_present(g);
}
int main(void) {
    SDL_Event e;
    g = gpu_create(); assert(g); assert(host_display_init(0, NULL, NULL));
    key(SDL_SCANCODE_UP, 1); key(SDL_SCANCODE_W, 1); key(SDL_SCANCODE_UP, 0);
    assert(host_input_buttons(0) == PAD_UP);
    key(SDL_SCANCODE_X, 1); key(SDL_SCANCODE_RETURN, 1);
    assert(host_input_buttons(0) == (PAD_UP | PAD_CROSS | PAD_START));
    key(SDL_SCANCODE_TAB, 1); assert(host_input_fast());
    key(SDL_SCANCODE_TAB, 0); assert(!host_input_fast());
    key(SDL_SCANCODE_F4, 1); assert(host_input_disc() == 4 && host_input_disc() == 0);
    key(SDL_SCANCODE_TAB, 1);
    memset(&e, 0, sizeof(e)); e.type = SDL_WINDOWEVENT; e.window.event = SDL_WINDOWEVENT_FOCUS_LOST;
    assert(SDL_PushEvent(&e) == 1); host_present(g); assert(!host_input_buttons(0));
    assert(!host_input_fast());
    int device = SDL_JoystickAttachVirtual(SDL_JOYSTICK_TYPE_GAMECONTROLLER, 6, 16, 0);
    assert(device >= 0); host_present(g);
    SDL_JoystickID id = SDL_JoystickGetDeviceInstanceID(device);
    button(id, SDL_CONTROLLER_BUTTON_A, 1); assert(host_input_buttons(0) == PAD_CROSS);
    key(SDL_SCANCODE_X, 1); button(id, SDL_CONTROLLER_BUTTON_A, 0);
    assert(host_input_buttons(0) == PAD_CROSS); key(SDL_SCANCODE_X, 0);
    memset(&e, 0, sizeof(e)); e.type = SDL_CONTROLLERAXISMOTION;
    e.caxis.which = id; e.caxis.axis = SDL_CONTROLLER_AXIS_TRIGGERLEFT; e.caxis.value = 20000;
    assert(SDL_PushEvent(&e) == 1); host_present(g); assert(host_input_buttons(0) == PAD_L2);
    int second = SDL_JoystickAttachVirtual(SDL_JOYSTICK_TYPE_GAMECONTROLLER, 6, 16, 0);
    assert(second >= 0); host_present(g);
    button(SDL_JoystickGetDeviceInstanceID(second), SDL_CONTROLLER_BUTTON_START, 1);
    assert(host_input_buttons(1) == PAD_START && host_input_buttons(0) == PAD_L2);
    assert(SDL_JoystickDetachVirtual(device) == 0); host_present(g);
    assert(!host_input_buttons(0)); assert(host_display_shutdown(g)); free(g); return 0;
}
