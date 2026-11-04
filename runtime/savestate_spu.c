#include <stddef.h>
#ifdef FF8_SS_PRIVATE
#include "savestate_internal.h"
/* Exact/full mixer schema. Lazy materialization, decode_memo and
 * live_block_fetches belong to FF8_PLAY and are deliberately unsupported.
 * No callback, backend, shadow, journal or diagnostic counter is persisted. */
void ss_spu(ss_io *io, spu_t *s) {
    unsigned i, j;
    /* v3 stores noise_timer as a fixed 17-bit phase, independent of rate. */
    ss_version(io, 3);
#ifdef FF8_PLAY
    io->failed = 1;
    return;
#endif
    S8(s->ram); S16(s->regs); S16(s->fifo);
    for (i = 0; i < 24; ++i) {
        voice_t *v = &s->voices[i];
        S32(v->adsr.level); S32(v->adsr.counter);
        for (j = 0; j < 2; ++j) { S32(v->volume[j].level); S32(v->volume[j].counter); }
        S32(v->phase); S32(v->delay); S32(v->position); S32(v->fraction); S32(v->decoded); S32(v->flags);
        S32(v->address); S16(v->samples); S16(v->history);
        if (v->position > 31 || v->phase > ENV_RELEASE || v->address > RAM_MASK) io->failed = 1;
    }
    for (j = 0; j < 2; ++j) { S32(s->master[j].level); S32(s->master[j].counter); }
    S32(s->transfer); S32(s->endx); S32(s->key_on); S32(s->key_off); S32(s->reverb_address);
    S32(s->reverb_size); S32(s->reverb_offset); S32(s->reverb_previous); S32(s->reverb_delayed); S32(s->reverb_dirty);
    S32(s->volume_work); S32(s->volume_sweep); S32(s->master_work); S32(s->master_sweep);
    S16(s->status); S16(s->noise); S32(s->noise_timer);
    S32(s->fifo_count); S32(s->capture); S32(s->fir_position); S32(s->reverb_tail);
    S16(s->reverb_input); S16(s->reverb_output); S16(s->output); S16(s->cd);
    S32(s->output_read); S32(s->output_count); S32(s->cd_read); S32(s->cd_count);
    S64(s->next_sample); S64(s->frames); S64(s->dropped); S64(s->cd_dropped); S64(s->cd_received);
    S64(s->key_ons); S64(s->blocks); S64(s->dma_words); S64(s->voice_frames); S64(s->cd_audible_frames);
    S64(s->batch_sample); S64(s->batch_due); S64(s->irq_sample); S64(s->irq_due); S32(s->batch_valid); S32(s->irq_valid);
    S64(s->voice_refresh); S64(s->voice_due); S64(s->voice_irq_due); S64(s->fetch_due); S64(s->fetch_irq_due);
    S32(s->voice_valid); S32(s->voice_cached);
    if (s->backend || s->control || s->replay || s->bulk || s->fifo_count > 32 || s->fir_position >= 64 ||
        s->output_read >= AUDIO_FRAMES || s->output_count > AUDIO_FRAMES || s->cd_read >= CD_FRAMES ||
        s->cd_count > CD_FRAMES || s->transfer > RAM_MASK || s->reverb_size > 512 * 1024 ||
        s->noise_timer < 0 || s->noise_timer > 0x1ffff) io->failed = 1;
}
#endif
