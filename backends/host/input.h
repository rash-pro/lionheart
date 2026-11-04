#ifndef FF8_HOST_INPUT_H
#define FF8_HOST_INPUT_H
#include "sio.h"

typedef struct {
    int dualshock, rumble, scripted;
    int analog_pad[2];
    int32_t controller_id[2];
} host_input_t;
void host_input_sync(cpu_t *, host_input_t *);
#endif
