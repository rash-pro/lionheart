#include "input.h"
#include "display.h"

void host_input_sync(cpu_t *c, host_input_t *input) {
    unsigned port;
    for (port = 0; port < 2; ++port) {
        uint8_t axes[4];
        int analog = input->dualshock && host_input_controller(port) && (port || !input->scripted);
        int32_t id = host_input_controller_id(port);
        if (analog != input->analog_pad[port] || (analog && id != input->controller_id[port])) {
            sio_set_pad(c, port, 1, analog);
            input->analog_pad[port] = analog;
        }
        input->controller_id[port] = id;
        host_input_axes(port, axes);
        sio_set_axes(c, port, axes);
        sio_set_buttons(c, port, host_input_buttons(port));
        host_input_feedback(port, sio_pad_output(c, port), analog && input->rumble);
    }
    sio_input_vblank(c);
}
