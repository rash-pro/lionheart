#include <stddef.h>
#if defined(FF8_SAVESTATES)
#include "savestate_internal.h"
#include <errno.h>

cpu_t *savestate_pending;
static int (*handler)(cpu_t *, void *);
static void *handler_context;
void savestate_handler(int (*fn)(cpu_t *, void *), void *ctx) { handler = fn; handler_context = ctx; savestate_pending = NULL; }
void savestate_request(cpu_t *c) { savestate_pending = c; }
void savestate_cancel(cpu_t *c) { if (savestate_pending == c) savestate_pending = NULL; }
int savestate_ready(cpu_t *c) {
    unsigned i;
    if (savestate_pending != c || !handler || !c->hw || c->hw->syncing || c->hw->in_exception ||
        c->fault_instructions || c->native_pin_count) return 0;
    for (i = 2; i <= c->dispatch_depth; ++i) if (c->trampolines[i].active) return 0;
    return 1;
}
int savestate_boundary(cpu_t *c) { savestate_pending = NULL; return handler(c, handler_context); }

static void identity(ss_io *io, savestate_identity_t *id) {
    S8(id->build); S32(id->exe); S32(id->disc); S32(id->modes); S8(id->disc_hash);
    S64(id->requested_vblank); S64(id->requested_instructions);
}
static void cpu_state(ss_io *io, cpu_t *c) {
    S32(c->r); S32(c->hi); S32(c->lo); S32(c->pc); S32(c->cop0); S32(c->gte_data); S32(c->gte_ctrl);
    S32(c->load_reg); S32(c->load_value); S32(c->branch_target); S32(c->branch_pc); S32(c->branch_return);
    S32(c->branch_pending); S32(c->branch_kind); S32(c->in_delay_slot); S32(c->flow); S32(c->flow_kind); S32(c->flow_return);
    S64(c->cycles); S64(c->next_event); S64(c->last_tick_cycles); S32(c->last_tick_pc); S32(c->tick_seen);
    S32(c->ram_limit); S32(c->ram_top); S32(c->memory_ticked); S32(c->memory_exit); S32(c->fault_counted);
    S32(c->seed); S32(c->heap_next); S32(c->heap_end); S32(c->event_next); S32(c->thread_next); S32(c->pad_clear);
    S64(c->interp_instructions); S64(c->recomp_instructions); S64(c->budget_recompiled); S64(c->idle_instructions);
    if (c->load_reg >= 32 || c->branch_pending > 1 || c->branch_kind > 2 || c->flow_kind > 2 ||
        c->in_delay_slot > 1 || (c->pc & 3) || c->ram_top > 0x800000u || c->ram_limit > 0x800000u) io->failed = 1;
}
static void hardware(ss_io *io, struct hw *h) {
    unsigned i;
    S64(h->deadline); S64(h->next_device); S64(h->media_bound); S32(h->i_stat); S32(h->i_mask);
    for (i = 0; i < 3; ++i) {
        hw_timer_t *t = &h->timers[i];
        S16(t->count); S16(t->mode); S16(t->target); S32(t->fired); S32(t->released); S64(t->fraction); S64(t->cycle);
    }
    S16(h->sio.mode); S16(h->sio.control); S16(h->sio.baud); S8(h->sio.rx); S8(h->sio.step);
    S32(h->sio.ready); S32(h->sio.ack); S32(h->sio.irq); S64(h->sio.due); S64(h->sio.ack_due);
    S64(h->now); S64(h->next_video); S64(h->video_edge); S64(h->video_start); S64(h->vblanks);
    S32(h->line); S32(h->hblank); S32(h->vblank); S32(h->video_pal);
    S64(h->vblank_edge); S64(h->vblank_time); S64(h->vblank_start); S32(h->vblank_pal); S32(h->vblank_valid);
    S64(h->timer_due); S32(h->timers_dirty);
    if (h->line > 313 || h->hblank > 1 || h->vblank > 1 || h->video_pal > 1) io->failed = 1;
}
static void bios(ss_io *io, struct hw *h) {
    unsigned i;
    for (i = 0; i < BIOS_EVENTS; ++i) {
        bios_event_t *e = &h->events[i]; S32(e->class_id); S32(e->spec); S32(e->mode); S32(e->handler); S32(e->state);
    }
    for (i = 0; i < BIOS_HEAP_BLOCKS; ++i) { S32(h->heap[i].address); S32(h->heap[i].size); S32(h->heap[i].used); }
    for (i = 0; i < BIOS_THREADS; ++i) {
        S32(h->thread_pipeline[i].pc); S32(h->thread_pipeline[i].load_reg); S32(h->thread_pipeline[i].load_value);
        S32(h->thread_pipeline[i].branch_target); S32(h->thread_pipeline[i].branch_pc); S32(h->thread_pipeline[i].branch_return);
        S32(h->thread_pipeline[i].branch_pending); S32(h->thread_pipeline[i].branch_kind);
        if (h->thread_pipeline[i].load_reg > 31 || h->thread_pipeline[i].branch_pending > 1) io->failed = 1;
    }
    S32(h->exit_hook); S32(h->auto_ack); S32(h->rcnt_ack); S32(h->pad_buffer); S32(h->pad_size);
    S32(h->pad_started); S32(h->card_started); S32(h->card_auto); S32(h->bios_cd);
    for (i = 0; i < 2; ++i) {
        bios_card_t *c = &h->cards[i];
        S64(c->due); S32(c->status); S32(c->sector); S32(c->buffer); S32(c->spec); S32(c->software_spec);
        S32(c->file_event); S32(c->length); S32(c->file); S32(c->operation); S32(c->allow_new); S32(c->software); S8(c->data);
    }
    S32(h->card_channel); S32(h->card_allow_new);
    for (i = 0; i < 16; ++i) {
        bios_file_t *f = &h->files[i];
        S32(f->used); S32(f->slot); S32(f->block); S32(f->mode); S32(f->busy); S32(f->error); S32(f->offset); S32(f->size);
        if (f->slot > 1 || f->block > 15) io->failed = 1;
    }
    S32(h->file_errno); S32(h->find_slot); S32(h->find_index); S8(h->find_pattern);
    if (h->find_slot > 1 || h->find_index > 16 || !memchr(h->find_pattern, 0, sizeof(h->find_pattern))) io->failed = 1;
}
static void payload(ss_io *io, cpu_t *c, unsigned id) {
    if (id <= 4) ss_version(io, 1);
    switch (id) {
    case 1: cpu_state(io,c); break;
    case 2: S8(c->ram); S8(c->scratch); S8(c->mmio); break;
    case 3: hardware(io,c->hw); break;
    case 4: bios(io,c->hw); break;
    case 5: ss_gpu(io,c->hw->gpu); break;
    case 6: ss_spu(io,c->hw->spu); break;
    case 7: ss_mdec(io,&c->hw->mdec); break;
    case 8: ss_dma(io,c->hw); break;
    case 9: ss_cdrom(io,&c->hw->cd); break;
    case 10: ss_sio(io,c); break;
    }
}
static void sections(ss_io *io, cpu_t *c) {
    unsigned expected;
    for (expected = 1; expected <= 10 && !io->failed; ++expected) {
        uint32_t id = expected, version = 1, size = 0;
        uint8_t hash[20] = {0}, actual[20];
        size_t head = io->pos, start, end;
        S32(id); S32(version); S32(size); S8(hash); start = io->pos;
        io->section = id; io->version = version;
        if (io->reading) {
            size_t limit = io->size;
            if (io->failed || id != expected || size > limit - start) { io->failed = 1; break; }
            code_sha1(io->data + start, size, actual);
            if (memcmp(hash, actual, 20)) { io->failed = 1; break; }
            io->size = start + size; payload(io, c, id);
            if (io->pos != io->size) io->failed = 1;
            io->size = limit;
        } else {
            payload(io, c, id); version = io->version; end = io->pos; size = (uint32_t)(end - start);
            if (io->failed) break;
            code_sha1(io->data + start, size, hash);
            io->pos = head; S32(id); S32(version); S32(size); S8(hash);
            io->pos = io->size = end;
        }
    }
}
int savestate_write(cpu_t *c, const char *path, const savestate_identity_t *identity_value, char *error, size_t error_size) {
    ss_io buffer = {0}, *io = &buffer;
    savestate_identity_t id = *identity_value;
    uint8_t magic[8] = {'F','F','8','S','A','V','E',0}, hash[20] = {0};
    uint32_t version = 1, size = 0;
    FILE *f = NULL;
    char *tmp = malloc(strlen(path) + 5);
    int ok = 0;
    if (!tmp) goto done;
    if (!c->hw || c->hw->syncing || c->hw->in_exception || c->fault_instructions || c->native_pin_count || c->dispatch_depth > 1) goto done;
    S8(magic); S32(version); S32(size); S8(hash);
    identity(io,&id); sections(io,c);
    if (io->failed) goto done;
    size = (uint32_t)io->size;
    code_sha1(io->data + 36, size - 36, hash);
    io->pos = 12; S32(size); S8(hash);
    snprintf(tmp, strlen(path) + 5, "%s.tmp", path);
    f = fopen(tmp, "wb");
    if (!f) goto done;
    ok = fwrite(io->data, 1, size, f) == size;
    if (fclose(f)) ok = 0;
    f = NULL;
    if (ok && rename(tmp, path)) ok = 0;
    if (!ok) remove(tmp);
done:
    if (!ok) snprintf(error, error_size, "save failed (unsafe boundary, unsupported device state, or I/O): %s", strerror(errno));
    free(tmp); free(io->data); return ok;
}
void savestate_dispose(cpu_t *c) {
    if (c) {
        sio_destroy(c); hw_destroy(c);
        free(c->interp_profile); free(c->overlay_profile); free(c->pc_profile);
        free(c);
    }
}
cpu_t *savestate_read(const cpu_t *old, const char *path, savestate_identity_t *wanted,
                      savestate_disc_open_t open_disc, void *context, char *error, size_t error_size) {
    ss_io buffer = {0}, *io = &buffer;
    savestate_identity_t id;
    cpu_t *c = NULL;
    FILE *f = fopen(path,"rb");
    uint8_t magic[8], hash[20], actual[20];
    uint32_t version, size;
    long length;
    snprintf(error, error_size, "invalid, corrupt, truncated, or unsupported section version in save state");
    if (!f) { snprintf(error,error_size,"%s: %s",path,strerror(errno)); return NULL; }
    if (fseek(f,0,SEEK_END) || (length = ftell(f)) < 36 || length > SAVESTATE_MAX || fseek(f,0,SEEK_SET)) goto fail;
    io->size = (size_t)length; io->reading = 1; io->data = malloc(io->size);
    if (!io->data || fread(io->data,1,io->size,f) != io->size) goto fail;
    S8(magic); S32(version); S32(size); S8(hash);
    if (memcmp(magic,"FF8SAVE\0",8) || version != 1 || size != io->size) goto fail;
    code_sha1(io->data + 36, io->size - 36, actual);
    if (memcmp(hash,actual,20)) goto fail;
    identity(io,&id);
    if (io->failed) goto fail;
    if (memcmp(id.build,wanted->build,64) || id.exe != wanted->exe || id.modes != wanted->modes) {
        snprintf(error,error_size,"save state belongs to a different build, EXE, or feature mode"); goto fail;
    }
    if (id.disc > 4) goto fail;
    c = malloc(sizeof(*c));
    if (!c) goto fail;
    cpu_init(c); hw_get(c); sio_init(c);
    c->entries = old->entries; c->entry_count = old->entry_count; c->entry_regions = old->entry_regions;
    c->overlays = old->overlays; c->overlay_count = old->overlay_count;
    c->interpreter_only = old->interpreter_only; c->instruction_limit = old->instruction_limit; c->stall_limit = old->stall_limit;
    c->log = old->log; c->log_context = old->log_context;
    c->hw->present = old->hw->present; c->hw->present_context = old->hw->present_context;
    if (id.disc && (!open_disc || !open_disc(c,id.disc,id.disc_hash,context,error,error_size))) goto fail;
    sections(io,c);
    if (io->failed || io->pos != io->size) {
        snprintf(error,error_size,"unsupported save state section %u version %u, or invalid size/payload",io->section,io->version);
        goto fail;
    }
    *wanted = id;
    fclose(f); free(io->data); return c;
fail:
    fclose(f); free(io->data); savestate_dispose(c); return NULL;
}
#endif
