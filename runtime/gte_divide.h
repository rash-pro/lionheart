#ifndef FF8_GTE_DIVIDE_H
#define FF8_GTE_DIVIDE_H
#include <stdint.h>
/* The division RTPS and RTPT make, H over SZ3 in 1.16, without flags: for host
 * code that projects a view-space position again as the GTE would. Kept out
 * of gte.h, which modules compile against. */
uint32_t gte_divide(uint32_t h, uint32_t z);
#endif
