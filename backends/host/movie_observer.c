/* Host linker observers. Read original state before forwarding exactly once. */
#include "movies.h"
#include "hw.h"
uint32_t __real_cdrom_dma(cpu_t *);
void __real_mdec_write(mdec_t *, uint32_t, uint64_t);
uint32_t __wrap_cdrom_dma(cpu_t *c) {
    if (c->hw->cd.data_pos == 0) host_movies_sector(c);
    return __real_cdrom_dma(c);
}
void __wrap_mdec_write(mdec_t *m, uint32_t value, uint64_t now) {
    if (!m->opcode && !m->count && (value >> 29) == 1) host_movies_decode();
    __real_mdec_write(m, value, now);
}
