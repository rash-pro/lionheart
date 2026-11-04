#include "state_bridge.h"
#include <stdio.h>
host_state_bridge_t *host_state_bridge_open(const char *port) {
    (void)port; fprintf(stderr,"State bridge disabled in this build\n"); return NULL;
}
void host_state_bridge_frame(host_state_bridge_t *b,const cpu_t *c,uint64_t v) { (void)b; (void)c; (void)v; }
void host_state_bridge_restore(host_state_bridge_t *b,const cpu_t *c,uint64_t v) { (void)b; (void)c; (void)v; }
void host_state_bridge_event(void *b,const char *j) { (void)b; (void)j; }
void host_state_bridge_close(host_state_bridge_t *b) { (void)b; }
