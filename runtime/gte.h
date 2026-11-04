#ifndef FF8_GTE_H
#define FF8_GTE_H
#include "cpu.h"

/* Static command fields are decoded by the emitter. shift is 0 or 12.
 * These entry points preserve gte_cmd's flags, count and timing contract. */
void gte_rtps(cpu_t *c, unsigned shift, int lm);
void gte_nclip(cpu_t *c);
void gte_op(cpu_t *c, unsigned shift, int lm);
void gte_dpcs(cpu_t *c, unsigned shift, int lm);
void gte_intpl(cpu_t *c, unsigned shift, int lm);
void gte_mvmva(cpu_t *c, unsigned shift, int lm, unsigned mx, unsigned vs, unsigned cv);
void gte_ncds(cpu_t *c, unsigned shift, int lm);
void gte_cdp(cpu_t *c, unsigned shift, int lm);
void gte_ncdt(cpu_t *c, unsigned shift, int lm);
void gte_nccs(cpu_t *c, unsigned shift, int lm);
void gte_cc(cpu_t *c, unsigned shift, int lm);
void gte_ncs(cpu_t *c, unsigned shift, int lm);
void gte_nct(cpu_t *c, unsigned shift, int lm);
void gte_sqr(cpu_t *c, unsigned shift, int lm);
void gte_dcpl(cpu_t *c, unsigned shift, int lm);
void gte_dpct(cpu_t *c, unsigned shift, int lm);
void gte_avsz3(cpu_t *c);
void gte_avsz4(cpu_t *c);
void gte_rtpt(cpu_t *c, unsigned shift, int lm);
void gte_gpf(cpu_t *c, unsigned shift, int lm);
void gte_gpl(cpu_t *c, unsigned shift, int lm);
void gte_ncct(cpu_t *c, unsigned shift, int lm);
#endif
