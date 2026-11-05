#ifndef FF8_FRAME_PROVENANCE_H
#define FF8_FRAME_PROVENANCE_H
#include <stdint.h>
struct cpu;
/* Diagnostic observations, never interpolation authorization. Inline CPU RAM
 * stores and direct GTE register accesses are outside this observer. */
void frame_provenance_projection(const struct cpu *, unsigned, uint32_t,
                                 uint32_t, unsigned, uint64_t);
void frame_provenance_write(unsigned, int);
void frame_provenance_dma(const struct cpu *, unsigned, const uint8_t *, unsigned, uint64_t);
#endif
