#ifndef FF8_HOST_STATE_BRIDGE_H
#define FF8_HOST_STATE_BRIDGE_H
#include "cpu.h"
typedef struct host_state_bridge host_state_bridge_t;
/* A successful fd:N open consumes N and owns its duplicate. On failure N is
 * retained by the caller. Listener/init failures are optional-feature warnings. */
host_state_bridge_t *host_state_bridge_open(const char *port);
void host_state_bridge_frame(host_state_bridge_t *, const cpu_t *, uint64_t vblank);
void host_state_bridge_event(void *, const char *json);
void host_state_bridge_restore(host_state_bridge_t *, const cpu_t *, uint64_t vblank);
void host_state_bridge_close(host_state_bridge_t *);
#endif
