#include <stddef.h>
#if defined(FF8_SAVESTATES)
#include "savestate_internal.h"
void ss_mdec(ss_io *io, mdec_t *m) {
    unsigned i;
    ss_version(io, 1);
    S16(m->input); S32(m->head); S32(m->count); S32(m->remaining); S32(m->table_pos); S32(m->opcode);
    for (i = 0; i < 32; ++i) { S32(m->fifo[i].word); S16(m->fifo[i].offset); S8(m->fifo[i].block); }
    S32(m->fifo_head); S32(m->fifo_count); S32(m->command); S32(m->control); S16(m->idle_count);
    S8(m->quant); S8(m->output); S16(m->scale); S16(m->blocks);
    S32(m->block); S32(m->coefficient); S32(m->qscale); S32(m->output_block);
    S32(m->output_pos); S32(m->output_size); S64(m->due); S64(m->blocks_decoded); S64(m->words_in); S64(m->words_out);
    if (m->head >= 64 || m->count > 64 || m->fifo_head >= 32 || m->fifo_count > 32 ||
        m->table_pos > 128 || m->opcode > 7 || m->qscale > 63 || m->output_block > 5 || m->block > 5 || m->coefficient > 64 || m->output_size > 192 || m->output_pos > 192)
        io->failed = 1;
}
#endif
