/* Verify the real-disc probe starts with neutral sticks and released triggers. */
#include "display.h"
#include "input.h"
#include "hw.h"
#include <SDL.h>
#include <assert.h>
#include <stdlib.h>

void __wrap_host_present(gpu_t *);
void __wrap_host_input_sync(cpu_t *, host_input_t *);
void __real_host_present(gpu_t *g) { host_present(g); }
void __real_host_input_sync(cpu_t *c, host_input_t *input) { host_input_sync(c, input); }
void __real_sio_write(cpu_t *c, uint32_t address, uint32_t value) { sio_write(c, address, value); }
uint32_t __real_sio_read(cpu_t *c, uint32_t address) { return sio_read(c, address); }

int main(void) {
    cpu_t *c = malloc(sizeof(*c)); assert(c); cpu_init(c); sio_init(c);
    host_input_t input = {.dualshock = 1};
    assert(host_display_init(0, NULL, NULL)); host_input_dualshock(1);
    for (unsigned frame = 1; frame <= 2; ++frame) {
        c->hw->vblanks = frame;
        __wrap_host_present(c->hw->gpu); __wrap_host_input_sync(c, &input);
        SDL_GameController *controller = SDL_GameControllerFromInstanceID(host_input_controller_id(0));
        assert(controller);
        SDL_Joystick *joystick = SDL_GameControllerGetJoystick(controller);
        assert(SDL_JoystickGetAxis(joystick, SDL_CONTROLLER_AXIS_TRIGGERLEFT) == -32768);
        assert(SDL_JoystickGetAxis(joystick, SDL_CONTROLLER_AXIS_TRIGGERRIGHT) == -32768);
        assert(SDL_GameControllerGetAxis(controller, SDL_CONTROLLER_AXIS_TRIGGERLEFT) == 0);
        assert(SDL_GameControllerGetAxis(controller, SDL_CONTROLLER_AXIS_TRIGGERRIGHT) == 0);
        assert(!(host_input_buttons(0) & (PAD_L2 | PAD_R2)));
    }
    assert(host_display_shutdown(c->hw->gpu)); sio_destroy(c); hw_destroy(c); free(c);
    return 0;
}
