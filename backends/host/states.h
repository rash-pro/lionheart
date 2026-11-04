#ifndef FF8_HOST_STATES_H
#define FF8_HOST_STATES_H
#include "input.h"
enum { HOST_CARDS_LIVE, HOST_CARDS_SNAPSHOT };
int host_states_init(cpu_t *, const char *dir, const char *card, const char *load, const char *save,
                     uint64_t at, const char **discs, unsigned count, uint32_t exe, unsigned modes, int full_disc, int cards);
void host_states_vblank(cpu_t *);
void host_states_disc(cpu_t *, unsigned);
int host_states_loading(void);
int host_states_apply(cpu_t **, host_input_t *);
int host_states_failed(void);
void host_states_shutdown(void);
void host_states_recovery(void (*callback)(cpu_t *, unsigned));
unsigned host_states_slot(void);
void host_states_slot_step(int direction);
int host_states_slot_available(unsigned, char *date, size_t);
#endif
