#include <stddef.h>
#if defined(FF8_SAVESTATES)
#include "savestate_internal.h"
void ss_dma(ss_io *io, struct hw *h) {
    unsigned i;
    ss_version(io, 1);
    S32(h->dpcr); S32(h->dicr);
    for (i = 0; i < 7; ++i) {
        S32(h->dma[i].madr); S32(h->dma[i].bcr); S32(h->dma[i].chcr);
        S64(h->dma[i].due); S64(h->dma[i].completions);
    }
    for (i = 0; i < 2; ++i) {
        S32(h->media_dma[i].remaining); S32(h->media_dma[i].block_left); S32(h->media_dma[i].grant);
    }
    S32(h->spu_dma.remaining); S32(h->spu_dma.block_left); S64(h->spu_dma.word_due);
    S32(h->fence_lo); S32(h->fence_hi);
}
#endif
