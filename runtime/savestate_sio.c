#include <stddef.h>
#ifdef FF8_SS_PRIVATE
#include "savestate_internal.h"
void ss_sio(ss_io *io, cpu_t *cpu) {
    sio_state_t *s = state(cpu, 1);
    uint32_t count = (uint32_t)s->script_count, pos = (uint32_t)s->script_pos;
    unsigned i;
    ss_version(io, 1);
    for (i = 0; i < 2; ++i) {
        struct pad *p = &s->pads[i];
        S16(p->buttons); S32(p->connected); S32(p->dualshock); S32(p->config); S32(p->analog);
        S32(p->command); S32(p->step); S32(p->length); S8(p->reply); S8(p->rumble); S8(p->axes);
        S8(p->small_motor); S8(p->large_motor);
        if (p->step > 9 || p->length > 9 || p->connected > 1 || p->dualshock > 1 || p->analog > 1 || p->config > 1) io->failed = 1;
        s->cards[i] = ss_memcard(io, s->cards[i]);
    }
    S8(s->fifo); S8(s->last_rx); S8(s->tx); S8(s->pending_tx); S32(s->count); S32(s->pending);
    S32(s->latched_enable); S32(s->device); S32(count); S32(pos);
    if (count > 65536 || pos > count || s->count > 8 || s->device > 3) { io->failed = 1; return; }
    if (io->reading) {
        s->script_count = count; s->script_pos = pos;
        if (count) {
            s->script = calloc(count, sizeof(*s->script));
            if (!s->script) { io->failed = 1; return; }
        }
    }
    for (i = 0; i < count; ++i) {
        S64(s->script[i].frame); S16(s->script[i].buttons); S32(s->script[i].disc);
        if (s->script[i].disc > 4 || (i && s->script[i].frame <= s->script[i-1].frame)) io->failed = 1;
    }
    S16(s->script_buttons); S32(s->scripted); S32(s->disc_request);
    if (s->scripted < 0 || s->scripted > 1 || s->disc_request > 4) io->failed = 1;
}
void ss_sio_input(cpu_t *c, int analog[2], int *scripted) {
    sio_state_t *s = state(c, 1);
    unsigned i;
    for (i = 0; i < 2; ++i) analog[i] = s->pads[i].connected && s->pads[i].dualshock;
    *scripted = s->scripted;
}
void ss_sio_live_cards(cpu_t *next, cpu_t *current, unsigned changed) {
    sio_state_t *restored = state(next,0), *live = state(current,0);
    struct hw *h = next->hw;
    unsigned i;
    /* A dispatcher boundary is not an idle peripheral boundary. Cancel the
     * restored card selection as a disconnected device, retaining pending TX
     * completion and RX readiness so the guest can observe failure and retry.
     * A pad transaction, including analog negotiation, is left untouched. */
    if (restored->device == 2 || (restored->device == 0 && (h->sio.control & 2) &&
        ((h->sio.due != HW_NEVER && restored->tx == 0x81) ||
         (restored->pending && restored->pending_tx == 0x81)))) {
        restored->device = 3; restored->last_rx = 0xff;
        memset(restored->fifo,0xff,sizeof(restored->fifo));
        h->sio.ack = 0; h->sio.ack_due = HW_NEVER;
    }
    for (i = 0; i < 2; ++i) {
        bios_card_t *job = &h->cards[i];
        int pending = job->due != HW_NEVER;
        memcard_close(restored->cards[i]);
        restored->cards[i] = live->cards[i]; live->cards[i] = NULL;
        ss_card_live_reset(restored->cards[i],pending || (changed & (1u << i)));
        if (pending) {
            /* Complete through the normal BIOS error/event path on resumption.
             * No old buffered write, ACK, or auto-format is replayed to disk. */
            job->due = HW_NEVER; job->status = restored->cards[i] ? 0x21 : 0x11;
            job->spec = restored->cards[i] ? 0x2000 : 0x100;
            job->software_spec = job->software ? job->spec : 0; job->file_event = 0;
            if ((job->operation == BIOS_CARD_FILE_READ || job->operation == BIOS_CARD_FILE_WRITE) && job->file < 16) {
                h->files[job->file].busy = 0; h->files[job->file].error = h->file_errno = 5;
            }
        }
    }
    hw_reschedule(next);
}
#endif
