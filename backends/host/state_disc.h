#ifndef FF8_HOST_STATE_DISC_H
#define FF8_HOST_STATE_DISC_H
#include "hw.h"
/* No guest CD reads/counters or FILE positions are changed. Cache is session-local. */
int host_state_disc_hash(const hw_cd_t *, int full, uint8_t hash[20]);
void host_state_disc_clear(void);
#endif
