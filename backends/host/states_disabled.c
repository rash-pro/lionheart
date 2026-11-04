#include "states.h"
#include "display.h"
#include "osd.h"
void host_states_recovery(void (*callback)(cpu_t *, unsigned)) { (void)callback; }
#include <stdio.h>
int host_states_init(cpu_t *c, const char *dir, const char *card, const char *load, const char *save,
                     uint64_t at, const char **discs, unsigned count, uint32_t exe, unsigned modes, int full_disc, int cards) {
    (void)c; (void)dir; (void)card; (void)discs; (void)count; (void)exe; (void)modes; (void)full_disc; (void)cards;
    if (load || save || at) { fprintf(stderr,"Save states disabled in this build\n"); return 0; }
    return 1;
}
/* F5-F8 only say that this build has no save states. */
void host_states_vblank(cpu_t *c) { (void)c; if (host_input_state()) host_osd_event(HOST_OSD_UNAVAILABLE,0,NULL); }
void host_states_disc(cpu_t *c, unsigned disc) { (void)c; (void)disc; }
int host_states_loading(void) { return 0; }
int host_states_apply(cpu_t **c, host_input_t *input) { (void)c; (void)input; return 0; }
int host_states_failed(void) { return 0; }
void host_states_shutdown(void) {}
unsigned host_states_slot(void) { return 0; }
void host_states_slot_step(int direction) { (void)direction; host_osd_event(HOST_OSD_UNAVAILABLE,0,NULL); }
int host_states_slot_available(unsigned slot,char *date,size_t capacity) { (void)slot; if (capacity) *date=0; return 0; }
