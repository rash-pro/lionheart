/* Deferred PCM is a chronological full-model replay.
 * Main state owns the live calendar; the shadow has no external callbacks. */
#ifdef FF8_PLAY
/* Bound the deferred journal memory. A full
 * journal materializes through the same exact replay before accepting writes. */
#define CONTROL_LOG_BYTES (1536u * 1024u)
#else
#define CONTROL_LOG_BYTES (2u * 1024u * 1024u)
#endif
typedef struct { uint64_t sample; uint32_t op, value; } control_event_t;
struct spu_control {
    spu_t *shadow;
    uint8_t *log;
    unsigned used, capacity, last, capture_dirty, reason;
    unsigned head;
    uint32_t loops;
#ifdef FF8_PLAY
    uint32_t live_pcm_valid;
    uint32_t pending_pitch;
    unsigned pitch_live;
    spu_t *voice_shadow;
    unsigned voice_valid, voice_head, voice_loops, partial_voice, voice_safe;
    unsigned voice_capture_dirty;
    uint32_t voice_reverb_dirty_base;
    uint32_t voice_needed;
    uint64_t voice_reads, voice_samples, voice_fallbacks, voice_pcm_frames;
    uint64_t voice_max_samples, sync_max_samples;
    unsigned background_active, background_credit;
    uint64_t background_calls, background_frames, background_syncs;
    unsigned background_max_bytes, background_max_samples;
#endif
    uint64_t reasons[7];
    uint64_t replay_voice_frames, replay_alias_spans, replay_fifo_spans;
    uint32_t reverb_dirty_base;
    uint64_t syncs, replay_frames, journal_full, aliases, alias_frames, fifo_spans, fifo_frames, pmon_frames;
};
static const spu_backend_t control_sink = {NULL, NULL};
static void control_uncache(spu_t *s) {
    if (s->control) s->control->loops = 0;
}

static void control_snapshot(spu_t *s) {
    spu_control_t *c = s->control;
    memcpy(c->shadow, s, sizeof(*s));
    c->shadow->control = NULL; c->shadow->replay = 1;
    c->shadow->backend = &control_sink; c->shadow->backend_context = NULL;
    c->shadow->irq = NULL; c->shadow->irq_context = NULL;
    c->loops = 0;
#ifdef FF8_PLAY
    c->live_pcm_valid = 0xffffffu;
    c->pending_pitch = 0; c->pitch_live = 1; c->background_active = c->background_credit = 0;
    c->voice_valid = c->partial_voice = 0;
    c->voice_safe = !s->fifo_count;
#endif
    c->head = c->used = c->capture_dirty = 0; c->last = UINT32_MAX; c->reverb_dirty_base = 0x80000;
}
static void control_create(spu_t *s) {
    spu_control_t *c;
    if (s->control) return;
    c = calloc(1, sizeof(*c));
    if (!c) return;
    c->shadow = malloc(sizeof(*s));
    c->capacity = CONTROL_LOG_BYTES;
    while (c->capacity >= 256u * 1024u) {
        c->log = malloc(c->capacity);
        if (c->log) break;
        c->capacity /= 2;
    }
    if (!c->shadow || !c->log) { free(c->shadow); free(c->log); free(c); return; }
    s->control = c; control_snapshot(s);
}
static void control_destroy(spu_t *s) {
    if (s->control) {
#ifdef FF8_PLAY
        free(s->control->voice_shadow);
#endif
        free(s->control->shadow); free(s->control->log); free(s->control);
    }
}
static void control_replay(spu_control_t *c, uint64_t cycles);
static unsigned control_apply_event(spu_control_t *c, unsigned at) {
    control_event_t e;
    memcpy(&e, c->log + at, sizeof(e)); at += sizeof(e);
    if (e.op < 0x400) spu_write(c->shadow, e.op, (uint16_t)e.value);
    else if (e.op == 0x400) (void)spu_dma_read(c->shadow);
    else if (e.op == 0x401) spu_dma_write(c->shadow, e.value);
    else {
        if (e.op == 0x402) spu_cd_input(c->shadow, (const int16_t *)(c->log + at), e.value);
        else {
            unsigned i;
            for (i = 0; i < e.value; ++i) {
                uint32_t word; memcpy(&word,c->log + at + i * 4u,4);
                spu_dma_write(c->shadow,word);
            }
        }
        at += (e.value * 4u + 7u) & ~7u;
    }
    return at;
}
static void control_publish(spu_t *s) {
    spu_control_t *c = s->control;
#ifdef FF8_PLAY
    s->blocks=c->shadow->blocks;s->key_ons=c->shadow->key_ons;
#endif
    /* Live CD/output queues, callbacks and the calendar do not materialize. */
    memcpy(s, c->shadow, offsetof(spu_t, output));
    s->batch_valid = s->irq_valid = s->voice_valid = s->voice_cached = 0;
    control_snapshot(s);
}
static void control_sync(spu_t *s) {
    spu_control_t *c = s->control;
    uint64_t first;
    if (!c || (!c->used && c->shadow->next_sample == s->next_sample)) return;
    first = c->shadow->frames;
    while (c->head < c->used) {
        control_event_t e;
        memcpy(&e, c->log + c->head, sizeof(e));
        control_replay(c, e.sample - SPU_SAMPLE_CYCLES);
        c->head = control_apply_event(c, c->head);
    }
    control_replay(c, s->next_sample - SPU_SAMPLE_CYCLES);
    ++c->reasons[c->reason]; ++c->syncs; c->replay_frames += c->shadow->frames - first;
#ifdef FF8_PLAY
    if (c->shadow->frames - first > c->sync_max_samples) c->sync_max_samples = c->shadow->frames - first;
#endif
    control_publish(s);
}
#ifdef FF8_PLAY
/* Only storage pressure requests canonical synthesis. Ordinary register and
 * clean transfer reads do not consume capture/reverb PCM. Keep replay chunks
 * bounded while draining bytes; a true dirty-RAM observer finishes the suffix. */
static void control_background(spu_t *s, unsigned budget) {
    spu_control_t *c = s->control;
    uint64_t target, first;
    unsigned events = 0, bytes = 0;
    if (!budget) return;
    if (!c->background_active && c->used - c->head < c->capacity * 3u / 4u) {
        c->background_credit = 0; return;
    }
    c->background_active = 1;
    if (budget > 1470) budget = 1470;
    /* Retain the original drain rate, but amortize tiny media advances.
     * Unspent credit survives the byte/event caps; it never borrows future
     * live samples. Storage pressure and publication reset the budget. */
    if (c->background_credit > 1470 - budget) c->background_credit = 1470;
    else c->background_credit += budget;
    if (c->background_credit < 128) return;
    budget = c->background_credit;
    first = c->shadow->frames;
    target = c->shadow->next_sample + (budget - 1u) * SPU_SAMPLE_CYCLES;
    if (target >= s->next_sample) target = s->next_sample - SPU_SAMPLE_CYCLES;
    while (c->head < c->used) {
        control_event_t e;
        memcpy(&e, c->log + c->head, sizeof(e));
        if (e.sample - SPU_SAMPLE_CYCLES > target) break;
        if (events++ == 64 || bytes >= 4096) { target = e.sample - SPU_SAMPLE_CYCLES; break; }
        control_replay(c, e.sample - SPU_SAMPLE_CYCLES);
        {
            unsigned old = c->head;
            c->head = control_apply_event(c, c->head); bytes += c->head - old;
        }
    }
    control_replay(c, target);
    ++c->background_calls;
    if (bytes > c->background_max_bytes) c->background_max_bytes = bytes;
    if (c->shadow->frames - first > c->background_max_samples) c->background_max_samples = (unsigned)(c->shadow->frames - first);
    c->background_frames += c->shadow->frames - first;
    c->replay_frames += c->shadow->frames - first;
    c->background_credit -= (unsigned)(c->shadow->frames - first);
    if (c->head == c->used && c->shadow->next_sample == s->next_sample) {
        ++c->background_syncs; ++c->syncs; ++c->reasons[0]; control_publish(s);
    } else if (c->used - c->head < c->capacity / 2u) {
        c->background_active = 0;
        c->background_credit = 0;
    }
}
#endif
static void control_materialize(spu_t *s, unsigned reason) {
    if (!s->control) return;
    s->control->reason = reason; control_sync(s); s->control->reason = 0;
}
#ifdef FF8_PLAY
/* Potentially audible decoder histories stay current without capture/reverb PCM.
 * PMON can therefore consume its predecessor immediately, and register reads
 * never replay an unbounded metadata history. Canonical RAM remains lazy. */
static int control_defer_pitch(const spu_t *s) {
    (void)s; return 0;
}
static int control_observe_voices(spu_t *s);
static void control_pitch_observe(spu_t *s, unsigned offset) {
    uint32_t pending = s->control ? s->control->pending_pitch : 0;
    if ((offset < 0x180 && ((offset & 15) == 12 || (offset & 15) == 14) &&
         (pending & (1u << (offset / 16)))) ||
        ((offset == 0x19c || offset == 0x19e) && pending))
        if (!control_observe_voices(s)) control_materialize(s, 6);
}
#endif
static void control_dma_observe(spu_t *s) {
    const spu_control_t *c = s->control;
    uint32_t a = s->transfer, b = (a + 2) & RAM_MASK;
    if (c && ((c->capture_dirty && (a < 0x1000 || b < 0x1000)) || a >= c->reverb_dirty_base || b >= c->reverb_dirty_base))
        control_materialize(s, 4);
}

static void control_log(spu_t *s, unsigned op, uint32_t value, const void *data, unsigned bytes) {
    spu_control_t *c = s->control;
    control_event_t e;
    unsigned size;
    uint32_t word = value;
    if (!c) return;
    if (op == 0x401) {
        if (c->last != UINT32_MAX && c->last >= c->head
#ifdef FF8_PLAY
            && (!c->voice_valid || c->last >= c->voice_head)
#endif
        ) {
            control_event_t previous;
            memcpy(&previous,c->log + c->last,sizeof(previous));
            size = sizeof(previous) + (((previous.value + 1) * 4u + 7u) & ~7u);
            if (previous.op == 0x403 && previous.value < 256u && previous.sample == s->next_sample && c->last + size <= c->capacity) {
                memcpy(c->log + c->last + sizeof(previous) + previous.value * 4u,&word,4);
                ++previous.value; memcpy(c->log + c->last,&previous,sizeof(previous));
                c->used = c->last + size; return;
            }
        }
        op = 0x403; value = 1; data = &word; bytes = 4;
    }
    size = sizeof(e) + ((bytes + 7u) & ~7u);
    if (c->used + size > c->capacity && c->head) {
        memmove(c->log, c->log + c->head, c->used - c->head);
#ifdef FF8_PLAY
        c->voice_valid=0;
#endif
        c->last = c->last >= c->head ? c->last - c->head : UINT32_MAX;
        c->used -= c->head; c->head = 0;
    }
    if (c->used + size > c->capacity) { ++c->journal_full; control_materialize(s, 1); }
    e.sample = s->next_sample; e.op = op; e.value = value;
    /* Classify after any capacity replay above, which resets eligibility. */
#ifdef FF8_PLAY
    /* Dirty transfer reads materialize before logging. Empty manual mode and
     * clean reads only move the transfer pointer; pending FIFO samples take
     * the canonical path in spu_advance before a scratch view can pass them. */
    if (op==0x1a8) c->voice_safe=0;
#endif
    c->last = c->used;
    memcpy(c->log + c->used, &e, sizeof(e));
    if (bytes) memcpy(c->log + c->used + sizeof(e), data, bytes);
    c->used += size;
}

static int control_alias(const spu_t *s, unsigned count) {
    /* Cached loops were admitted only after this configuration's alias proof.
     * Their sole block stays disjoint indefinitely; register/key/RAM writers
     * and materialization invalidate the cache before that proof can change. */
    uint32_t voices = 0xffffffu & ~s->control->loops;
    return voices && voices_alias(s, count, voices, s->control->capture_dirty, s->control->reverb_dirty_base);
}

/* Only the shadow uses these spans: callbacks are disabled, and no guest can
 * observe an intermediate voice state. PCM consumers and modulated carriers
 * retain the sample loop. All other voices retain exact ADPCM history through
 * voice_advance, after proving their fetches disjoint from capture/reverb writes. */
static void control_replay(spu_control_t *c, uint64_t cycles) {
    spu_t *s = c->shadow;
    while (s->next_sample <= cycles) {
        uint64_t remaining = (cycles - s->next_sample) / SPU_SAMPLE_CYCLES + 1;
        unsigned count = remaining > 4096 ? 4096 : (unsigned)remaining, n, ch;
        uint32_t pmon = flags(s, 0x190), control = REG(s, 0x1aa);
        uint32_t needed = (pmon >> 1) | ((REG(s, 0x1ac) & 12) ? 0xau : 0) |
            ((control & 0x4000) ? flags(s, 0x198) : 0);
        uint32_t independent;
#ifdef FF8_PLAY
        /* An OFF voice with zero ENVX cannot feed PCM or modulate its next
         * voice during this event-free span. Pending KON invalidates that
         * proof. Still advance its complete decoder/envelope/volume state. */
        uint32_t silent = 0;
        for (n = 0; n < 24; ++n)
            if (s->voices[n].phase == ENV_OFF && !s->voices[n].adsr.level &&
                !(s->key_on & (1u << n))) silent |= 1u << n;
        needed &= ~silent;
        pmon &= ~(silent << 1);
#endif
        independent = 0xffffffu & ~(needed | (pmon & ~1u));
        if (s->fifo_count) {
            ++c->replay_fifo_spans;
            spu_advance_legacy(s, s->next_sample + (count - 1) * SPU_SAMPLE_CYCLES);
        } else {
#ifdef FF8_PLAY
            uint32_t aliased = voices_alias_mask(s,count,independent,0,0x80000);
            independent &= ~aliased; c->replay_alias_spans += !!aliased;
#else
            unsigned aliased = 0;
            for (n = 0; n < 24; ++n) if ((independent & (1u << n)) &&
                voices_alias(s, count, 1u << n, 0, 0x80000)) {
                independent &= ~(1u << n); aliased = 1;
            }
            c->replay_alias_spans += aliased;
#endif
            for (n = 0; n < 24; ++n) if (independent & (1u << n)) {
                voice_t *v = &s->voices[n];
                if (s->key_off & (1u << n)) { v->phase = ENV_RELEASE; v->adsr.counter = 0; }
                if (s->key_on & (1u << n)) start_voice(s, n);
                if (!(control & 0x8000)) { v->phase = ENV_OFF; v->adsr.level = 0; }
                voice_advance(s, n, count);
                for (ch = 0; ch < 2; ++ch) if (s->volume_work[ch] & (1u << n))
                    volume_advance(&v->volume[ch], REG(s, n * 16 + ch * 2),
                        s->volume_sweep[ch] & (1u << n) ? count : 1);
                c->replay_voice_frames += count;
            }
            s->bulk = independent;
#ifdef FF8_PLAY
            {
                uint8_t active[24]; unsigned active_count = 0;
                for (n = 0; n < 24; ++n) if (!(independent & (1u << n))) active[active_count++] = (uint8_t)n;
                for (n = 0; n < count; ++n) { sample_active(s, active, active_count); s->next_sample += SPU_SAMPLE_CYCLES; }
            }
#else
            for (n = 0; n < count; ++n) { sample(s); s->next_sample += SPU_SAMPLE_CYCLES; }
#endif
            s->bulk = 0;
        }
    }
}

static void control_header(spu_t *s, unsigned n) {
    voice_t *v = &s->voices[n];
    uint32_t address = (uint32_t)REG(s, 0x1a4) * 8;
    if (address - v->address < 16) ram_irq(s, address);
    v->flags = s->ram[(v->address + 1) & RAM_MASK]; v->decoded = 1; ++s->blocks;
    if (v->flags & 4) {
        if (v->phase) trace_voice(s, n, "loop-start", v->address);
        REG(s, n * 16 + 14) = (uint16_t)(v->address / 8);
    }
}
static void control_loop(spu_t *s, unsigned n, unsigned step, unsigned count) {
    voice_t *v = &s->voices[n];
    uint16_t *r = &REG(s,n*16);
    unsigned total = v->position * 4096 + v->fraction + step * count;
    unsigned crossings = total / (28 * 4096), final = total % (28 * 4096);
    unsigned last_crossed = crossings && final < step;
    if (v->phase) adsr_advance(v,r[4],r[5],count);
    s->blocks += crossings - last_crossed;
    if (crossings) s->endx |= 1u << n;
    v->decoded = !last_crossed; v->position = final >> 12; v->fraction = final & 0xfff;
    return;
}
static void control_voice_slow(spu_t *s, unsigned n, unsigned count) {
    voice_t *v = &s->voices[n];
    uint16_t *r = &REG(s, n * 16);
    unsigned step = r[2] > 0x4000 ? 0x4000 : r[2], delay = v->delay < count ? v->delay : count;
    v->delay -= delay; count -= delay;
    while (count) {
        unsigned span = count, fraction;
        if (!v->decoded) control_header(s, n);
        if (!step) { adsr_advance(v, r[4], r[5], count); return; }
        if ((v->flags & 3) == 3 && r[7] == v->address / 8 &&
            s->ram[(v->address + 1) & RAM_MASK] == v->flags) {
            s->control->loops |= 1u << n;
            control_loop(s,n,step,count); return;
        }
        if (step) {
            unsigned distance = (28 - v->position) * 4096 - v->fraction;
            if (step * span > distance) span = (distance + step - 1) / step;
        }
        adsr_advance(v, r[4], r[5], span);
        fraction = v->fraction + step * span; v->position += fraction >> 12; v->fraction = fraction & 0xfff;
        count -= span;
        if (v->position >= 28) {
            v->position -= 28; v->decoded = 0;
            if (v->flags & 1) {
                if (v->phase) trace_voice(s, n, "loop-end", (uint32_t)r[7] * 8 & ~15u);
                s->endx |= 1u << n; v->address = (uint32_t)r[7] * 8 & ~15u;
                if (!(v->flags & 2) && !(flags(s, 0x194) & (1u << n))) { v->phase = ENV_OFF; v->adsr.level = 0; }
            } else v->address = (v->address + 16) & RAM_MASK;
        }
    }
}

/* A loop stays valid until a register, key, RAM write or materialization can
 * change its header, repeat address or pitch. Terminal fetches still run the
 * normal header/IRQ check on the next span. */
static void control_voice(spu_t *s, unsigned n, unsigned count) {
    if (s->control->loops & (1u << n)) {
        unsigned step = REG(s,n*16+4);
        if (step > 0x4000) step = 0x4000;
        if (!s->voices[n].decoded) control_header(s,n);
        control_loop(s,n,step,count);
    } else control_voice_slow(s,n,count);
}

#ifdef FF8_PLAY
/* The live caller already proved this span disjoint from generated RAM. A
 * self-loop then keeps that fetch proof until the existing key/register/RAM
 * invalidators clear it. OFF/zero voices need only metadata until reactivated. */
static void control_live_voice(spu_t *s, unsigned n, unsigned count) {
    uint32_t bit=1u<<n;
    if(s->voices[n].phase==ENV_OFF && !s->voices[n].adsr.level) {
        control_voice(s,n,count);
        s->control->live_pcm_valid&=~bit;
        return;
    }
    voice_advance(s,n,count);
    if(!(s->control->loops&bit)) {
        const voice_t *v=&s->voices[n];
        if((v->flags&3)==3 && REG(s,n*16+14)==v->address/8 &&
           s->ram[(v->address+1)&RAM_MASK]==v->flags) s->control->loops|=bit;
    }
}
#endif

/* Address-only reverb core, in the same channel/access order as reverb_core. */
static void control_reverb_irq(spu_t *s) {
    unsigned ch, i;
    int enabled = !!(REG(s, 0x1aa) & 0x80);
    if (s->reverb_dirty) reverb_offsets(s);
    for (ch = 0; ch < 2; ++ch) {
        if (enabled) for (i = 0; i < 2; ++i) {
            unsigned dest = (i ? 18 : 10) + ch, source = i ? 24 + (ch ^ 1) : 16 + ch;
            ram_irq(s, reverb_cached_address(s, s->reverb_previous[i * 2 + ch]));
            ram_irq(s, reverb_cached_address(s, s->reverb_offset[source]));
            ram_irq(s, reverb_cached_address(s, s->reverb_offset[dest]));
        }
        for (i = 0; i < 4; ++i) {
            static const unsigned comb[4] = {12,14,20,22};
            ram_irq(s, reverb_cached_address(s, s->reverb_offset[comb[i] + ch]));
        }
        for (i = 0; i < 2; ++i) {
            ram_irq(s, reverb_cached_address(s, s->reverb_delayed[i * 2 + ch]));
            if (enabled) ram_irq(s, reverb_cached_address(s, s->reverb_offset[26 + i * 2 + ch]));
        }
    }
}
static void control_keys(spu_t *s) {
    unsigned n, control = REG(s, 0x1aa);
    if ((control & 0x8000) && !(s->key_on | s->key_off)) return;
    if (s->backend->write) {
        if (s->key_off & 0xffff) s->backend->write(s->backend_context, 0x18c, (uint16_t)s->key_off);
        if (s->key_off >> 16) s->backend->write(s->backend_context, 0x18e, (uint16_t)(s->key_off >> 16));
        if ((control & 0x8000) && (s->key_on & 0xffff)) s->backend->write(s->backend_context, 0x188, (uint16_t)s->key_on);
        if ((control & 0x8000) && (s->key_on >> 16)) s->backend->write(s->backend_context, 0x18a, (uint16_t)(s->key_on >> 16));
    }
    s->control->loops &= ~(s->key_on | s->key_off);
#ifdef FF8_PLAY
    /* KON resets every decoder history before the next sample. */
    s->control->live_pcm_valid |= s->key_on;
#endif
    for (n = 0; n < 24; ++n) {
        voice_t *v = &s->voices[n];
        if (s->key_off & (1u << n)) { v->phase = ENV_RELEASE; v->adsr.counter = 0; }
        if (s->key_on & (1u << n)) start_voice(s, n);
        if (!(control & 0x8000)) { v->phase = ENV_OFF; v->adsr.level = 0; }
    }
    s->key_on = s->key_off = 0;
}
static void control_aux(spu_t *s) {
    unsigned ch, control = REG(s, 0x1aa), mode = (control >> 4) & 3, pos = s->fir_position;
    int16_t cd[2] = {0,0};
    s->status = (s->status & 0x40) | (control & 0x3f) | (mode >= 2 ? 0x80 | (mode == 2 ? 0x100 : 0x200) : 0);
    noise_advance(s,1);
    if (s->cd_count) { memcpy(cd, s->cd[s->cd_read], sizeof(cd)); s->cd_read = (s->cd_read + 1) % CD_FRAMES; --s->cd_count; }
    if ((control & 1) && (mul_sample(cd[0], (int16_t)REG(s, 0x1b0)) || mul_sample(cd[1], (int16_t)REG(s, 0x1b2)))) ++s->cd_audible_frames;
    if (REG(s, 0x1ac) & 12) {
        if ((control & 0x8040) == 0x8040 && !(s->status & 0x40)) {
            for (ch = 0; ch < 4; ++ch) ram_irq(s, s->capture + ch * 0x400);
            if (!(pos & 1)) control_reverb_irq(s);
        }
        s->control->capture_dirty = 1;
        if ((control & 0x80) && (uint32_t)REG(s, 0x1a2) * 8 < s->control->reverb_dirty_base) s->control->reverb_dirty_base = (uint32_t)REG(s, 0x1a2) * 8;
        s->capture = (s->capture + 2) & 0x3ff;
        if (s->capture & 0x200) s->status |= 0x800;
        if (!(pos & 1)) { s->reverb_address += 2; if (s->reverb_address > RAM_MASK) s->reverb_address = (uint32_t)REG(s, 0x1a2) * 8; }
    }
    s->fir_position = (pos + 1) & 63;
    if (s->output_count == AUDIO_FRAMES) { s->output_read = (s->output_read + 1) % AUDIO_FRAMES; --s->output_count; ++s->dropped; }
    memcpy(s->output[(s->output_read + s->output_count) % AUDIO_FRAMES], cd, sizeof(cd));
    ++s->output_count; ++s->frames; ++s->control_frames; ++s->bulk_frames;
}

/* Capture/reverb PCM stays deferred across this interval. Play also keeps the
 * noise clock current for a later PMON input, without mixing unused PCM. */
static void control_aux_span(spu_t *s, unsigned count) {
    unsigned control = REG(s,0x1aa), mode = (control >> 4) & 3, left = count;
    unsigned pos = s->fir_position, cores = (count + !(pos & 1)) / 2;
#ifdef FF8_PLAY
    noise_advance(s,count);
#endif
    if (s->output_count + count > AUDIO_FRAMES) {
        unsigned dropped = s->output_count + count - AUDIO_FRAMES;
        s->output_read = (s->output_read + dropped) % AUDIO_FRAMES;
        s->output_count -= dropped; s->dropped += dropped;
    }
    while (left) {
        unsigned at = (s->output_read + s->output_count) % AUDIO_FRAMES;
        unsigned part = AUDIO_FRAMES - at, i;
        if (part > left) part = left;
        if (s->cd_count) {
            if (part > CD_FRAMES - s->cd_read) part = CD_FRAMES - s->cd_read;
            if (part > s->cd_count) part = s->cd_count;
            memcpy(s->output[at],s->cd[s->cd_read],part * 4u);
            if (control & 1) for (i = 0; i < part; ++i)
                if (mul_sample(s->cd[s->cd_read+i][0],(int16_t)REG(s,0x1b0)) ||
                    mul_sample(s->cd[s->cd_read+i][1],(int16_t)REG(s,0x1b2))) ++s->cd_audible_frames;
            s->cd_read = (s->cd_read + part) % CD_FRAMES; s->cd_count -= part;
        } else memset(s->output[at],0,part * 4u);
        s->output_count += part; left -= part;
    }
    s->status = (s->status & 0x40) | (control & 0x3f) | (mode >= 2 ? 0x80 | (mode == 2 ? 0x100 : 0x200) : 0);
    if (REG(s,0x1ac) & 12) {
        uint32_t base = (uint32_t)REG(s,0x1a2) * 8;
        s->control->capture_dirty = 1;
        if (cores && (control & 0x80) && base < s->control->reverb_dirty_base) s->control->reverb_dirty_base = base;
        s->capture = (s->capture + count * 2) & 0x3ff;
        if (s->capture & 0x200) s->status |= 0x800;
        s->reverb_address = base + (s->reverb_address - base + cores * 2) % (0x80000 - base);
    }
    s->fir_position = (pos + count) & 63;
    s->frames += count; s->control_frames += count; s->bulk_frames += count;
}

#ifdef FF8_PLAY
/* Once observed, keep only PMON's dependency chain sample-granular. Independent
 * headers/envelopes still span the interval, and no capture/reverb PCM is mixed. */
static void control_pitch_span(spu_t *s, unsigned count) {
    uint32_t pmon=flags(s,0x190)&~1u, needed=pmon>>1, dependent, noise=flags(s,0x194);
    unsigned n, i;
    for(n=1;n<24;++n) if((pmon&(1u<<n)) && s->voices[n-1].phase==ENV_OFF &&
        !s->voices[n-1].adsr.level) pmon&=~(1u<<n);
    dependent=pmon|(pmon>>1);
    for(n=0;n<24;++n) if(!(dependent&(1u<<n))) {
        if(needed&(1u<<n))control_live_voice(s,n,count);
        else control_live_voice(s,n,count);
    }
    if(pmon) {
        s->control->pmon_frames+=count;
        for(i=0;i<count;++i) {
            int32_t previous=0;uint32_t work=dependent;
            control_aux(s);
            while(work) {
                uint32_t bit=work&-work;n=(unsigned)__builtin_ctz(work);work-=bit;
                if(needed&bit)previous=voice_sample_needed(s,n,previous,pmon,noise,1);
                else {
                    uint16_t saved=REG(s,n*16+4);
                    if(n&&(pmon&bit))REG(s,n*16+4)=(uint16_t)mul_sample((int16_t)saved,previous+32768);
                    control_live_voice(s,n,1);REG(s,n*16+4)=saved;previous=0;
                }
            }
        }
    } else control_aux_span(s,count);
    s->next_sample+=(uint64_t)(count-1u)*SPU_SAMPLE_CYCLES;
}

/* This view never synthesizes capture/reverb RAM. Every source fetch must be
 * disjoint from all deferred work areas, including historical ESA regions.
 * Decoder history is retained for every voice which can feed PMON anywhere in
 * the retained journal. Other voices need only exact positions and envelopes. */
/* KON resets decoder history. Before a source's first PMON consumption, only
 * metadata is needed up to its latest reset. Group writes at one sample: KON
 * may arrive after PMON in that group but is latched before any voice runs. */
static void control_voice_decode_from(const spu_control_t *c, const spu_t *q,
                                      unsigned at, uint64_t from[24]) {
    uint64_t latest[24],sample=q->next_sample;
    uint32_t pmon=flags(q,0x190),seen=0;
    unsigned n;
    for(n=0;n<24;++n)from[n]=latest[n]=sample;
    while(at<c->used) {
        control_event_t e;uint64_t next;
        memcpy(&e,c->log+at,sizeof(e));next=e.sample;
        if(next>sample) {
            uint32_t first=(pmon>>1)&~seen;
            for(n=0;n<24;++n)if(first&(1u<<n))from[n]=latest[n];
            seen|=first;sample=next;
        }
        do {
            uint32_t keys=0;
            memcpy(&e,c->log+at,sizeof(e));
            if(e.op==0x190)pmon=(pmon&0xff0000u)|(e.value&0xffffu);
            if(e.op==0x192)pmon=(pmon&0xffffu)|((e.value&255u)<<16);
            if(e.op==0x188)keys=e.value&0xffffu;
            if(e.op==0x18a)keys=(e.value&255u)<<16;
            for(n=0;keys;++n,keys>>=1)if(keys&1)latest[n]=sample;
            at+=sizeof(e);
            if(e.op>=0x402)at+=(e.value*4u+7u)&~7u;
            if(at==c->used)break;
            memcpy(&e,c->log+at,sizeof(e));
        } while(e.sample==sample);
    }
    /* Current consumers need valid decoder state for live pitch stepping too.
     * A pending same-sample KON will reset it before the next sample. Sources
     * which never consume PCM conservatively retain the whole history. */
    for(n=0;n<24;++n)if(((pmon>>1)&~seen)&(1u<<n))from[n]=latest[n];
}
static int control_voice_advance(spu_control_t *view, uint64_t until, uint32_t needed, const uint64_t from[24]) {
    spu_t *q=view->shadow;
    while(q->next_sample<=until) {
        uint64_t left=(until-q->next_sample)/SPU_SAMPLE_CYCLES+1;
        unsigned count=left>4096?4096:(unsigned)left,n;
        uint32_t pmon=flags(q,0x190),noise=flags(q,0x194),dependent;
        if(q->fifo_count ||
           voices_alias(q,count,0xffffff,view->capture_dirty,view->reverb_dirty_base)) return 0;
        q->control=view;control_keys(q);
        /* An off predecessor produces zero throughout this event-free span.
         * Its carrier therefore keeps its ordinary pitch. Retain decoder
         * history for future PMON use, but avoid a pointless sample loop. */
        for(n=1;n<24;++n)if((pmon&(1u<<n)) && q->voices[n-1].phase==ENV_OFF &&
            !q->voices[n-1].adsr.level)
            pmon&=~(1u<<n);
        dependent=(pmon|(pmon>>1))&0xffffff;
        for(n=0;n<24;++n)if(!(dependent&(1u<<n))) {
            if((needed&(1u<<n)) && q->next_sample>=from[n]) {voice_advance(q,n,count);view->voice_pcm_frames+=count;}
            else control_voice(q,n,count);
        }
        if(pmon) {
            unsigned sample;
            for(sample=0;sample<count;++sample) {
                int32_t previous=0;uint32_t work=dependent;
                noise_advance(q,1);
                while(work) {
                    uint32_t bit=work&-work;n=(unsigned)__builtin_ctz(work);work-=bit;
                    if(needed&bit) {previous=voice_sample_needed(q,n,previous,pmon,noise,!!((pmon>>1)&bit));++view->voice_pcm_frames;}
                    else {
                        uint16_t saved=REG(q,n*16+4);
                        if(n&&(pmon&bit)) REG(q,n*16+4)=(uint16_t)mul_sample((int16_t)saved,previous+32768);
                        control_voice(q,n,1);REG(q,n*16+4)=saved;previous=0;
                    }
                }
            }
        } else noise_advance(q,count);
        if(REG(q,0x1ac)&12) {
            view->capture_dirty=1;
            if((REG(q,0x1aa)&0x80) && (uint32_t)REG(q,0x1a2)*8<view->reverb_dirty_base)
                view->reverb_dirty_base=(uint32_t)REG(q,0x1a2)*8;
        }
        q->next_sample+=(uint64_t)count*SPU_SAMPLE_CYCLES;q->frames+=count;
        q->control=NULL;
    }
    return 1;
}
static int control_observe_voices(spu_t *s) {
    spu_control_t *c=s->control,view={0};spu_t *q;
    unsigned at,n;uint32_t needed,pmon;
    uint64_t before,from[24];
    if(!c->voice_shadow || !c->voice_safe) {++c->voice_fallbacks;return 0;}
    if(c->voice_valid && (c->voice_head<c->head ||
       c->voice_shadow->next_sample<c->shadow->next_sample))c->voice_valid=0;
    q=c->voice_valid?c->voice_shadow:c->shadow;
    pmon=flags(q,0x190);needed=c->voice_valid?c->voice_needed:0;needed|=pmon>>1;
    for(at=c->voice_valid?c->voice_head:c->head;at<c->used;) {
        control_event_t e;memcpy(&e,c->log+at,sizeof(e));at+=sizeof(e);
        if(e.op==0x190)pmon=(pmon&0xff0000)|(e.value&0xffff);
        if(e.op==0x192)pmon=(pmon&0xffff)|((e.value&0xff)<<16);
        needed|=pmon>>1;
        if(e.op>=0x402)at+=(e.value*4u+7u)&~7u;
    }
    if(!c->voice_valid || (needed&~c->voice_needed)) {
        memcpy(c->voice_shadow,c->shadow,sizeof(*s));c->voice_head=c->head;c->voice_loops=0;
        c->voice_capture_dirty=0;c->voice_reverb_dirty_base=0x80000;
    }
    q=c->voice_shadow;view.shadow=q;view.log=c->log;view.loops=c->voice_loops;
    /* The canonical prefix contains correct RAM. Mark work areas only after
     * the view skips their writes; future dirty areas cannot alias an earlier
     * fetch before capture/reverb has even been enabled. */
    view.capture_dirty=c->voice_capture_dirty;view.reverb_dirty_base=c->voice_reverb_dirty_base;
    before=q->frames;
    at=c->voice_head;control_voice_decode_from(c,q,at,from);
    while(at<c->used) {
        control_event_t e;memcpy(&e,c->log+at,sizeof(e));
        if(!control_voice_advance(&view,e.sample-SPU_SAMPLE_CYCLES,needed,from))goto fallback;
        /* Event application must have no control context: it must neither
         * append another journal nor recursively observe this scratch view. */
        q->control=NULL;at=control_apply_event(&view,at);view.loops=0;
    }
    if(!control_voice_advance(&view,s->next_sample-SPU_SAMPLE_CYCLES,needed,from))goto fallback;
    for(n=0;n<24;++n) {
        envelope_t volume[2];memcpy(volume,s->voices[n].volume,sizeof(volume));
        s->voices[n]=q->voices[n];memcpy(s->voices[n].volume,volume,sizeof(volume));
        REG(s,n*16+14)=REG(q,n*16+14);
    }
    s->blocks=q->blocks;s->key_ons=q->key_ons;
    s->endx=q->endx;s->batch_valid=s->irq_valid=s->voice_valid=s->voice_cached=0;
    c->loops=0;c->pending_pitch=0;c->pitch_live=1;c->partial_voice=1;
    c->voice_valid=1;c->voice_head=at;c->voice_loops=view.loops;c->voice_needed=needed;
    c->voice_capture_dirty=view.capture_dirty;c->voice_reverb_dirty_base=view.reverb_dirty_base;
    ++c->voice_reads;c->voice_samples+=q->frames-before;c->voice_pcm_frames+=view.voice_pcm_frames;
    if(q->frames-before>c->voice_max_samples)c->voice_max_samples=q->frames-before;
    return 1;
fallback:
    c->voice_pcm_frames+=view.voice_pcm_frames;
    c->voice_samples+=q->frames-before;
    if(q->frames-before>c->voice_max_samples)c->voice_max_samples=q->frames-before;
    q->control=NULL;c->voice_valid=0;c->voice_safe=0;++c->voice_fallbacks;return 0;
}
#endif

void spu_advance(spu_t *s, uint64_t cycles) {
    spu_control_t *c = s->control;
#ifdef FF8_PLAY
    uint64_t first_sample = s->next_sample;
#endif
    if (!c) {
        if (s->backend && !s->replay && s->next_sample <= cycles) {
            ++s->allocation_fallback_calls; s->allocation_fallback_frames += (cycles - s->next_sample) / SPU_SAMPLE_CYCLES + 1;
        }
        spu_advance_legacy(s, cycles); return;
    }
    while (s->next_sample <= cycles) {
        uint64_t remaining = (cycles - s->next_sample) / SPU_SAMPLE_CYCLES + 1;
        unsigned count = remaining > 183 ? 183 : (unsigned)remaining, n, ch;
        int irq = (REG(s, 0x1aa) & 0x8040) == 0x8040 && !(s->status & 0x40);
        if (irq) {
            uint64_t horizon = spu_irq_deadline(s);
            unsigned safe = (unsigned)((horizon - s->next_sample) / SPU_SAMPLE_CYCLES);
            if (safe) { if (count > safe) count = safe; irq = 0; }
            else count = 1;
        }
        if (s->fifo_count || control_alias(s, count)) {
            if (s->fifo_count) { ++c->fifo_spans; c->fifo_frames += count; }
            else { ++c->aliases; c->alias_frames += count; }
            control_materialize(s, s->fifo_count ? 3 : 2);
            s->control = NULL; spu_advance_legacy(s, s->next_sample + (count - 1) * SPU_SAMPLE_CYCLES); s->control = c;
            control_snapshot(s); continue;
        }
        /* The horizon proves the bulk interval cannot hit the IRQ address.
         * Its boundary retains exact address checks on the current sample. */
        control_keys(s);
#ifdef FF8_PLAY
        if(flags(s,0x190) && c->pitch_live && !(REG(s,0x1aa)&0x40)) {
            control_pitch_span(s,count);
        } else
#endif
        if (flags(s, 0x190)
#ifdef FF8_PLAY
            && !control_defer_pitch(s)
#endif
        ) {
            int32_t previous = 0;
            uint32_t pmon = flags(s, 0x190), noise = flags(s, 0x194);
            unsigned needed = (pmon & ~1u) >> 1;
            count = 1; ++c->pmon_frames;
            /* Noise is clocked before voice output in the reference sample. */
            control_aux(s);
            for (n = 0; n < 24; ++n) {
                if (needed & (1u << n)) previous = voice_sample_needed(s,n,previous,pmon,noise,1);
                else {
                    unsigned step = REG(s,n*16+4); uint16_t saved = (uint16_t)step;
                    if (n && (pmon & (1u << n))) { step = (uint16_t)mul_sample((int16_t)step, previous + 32768); REG(s,n*16+4) = (uint16_t)step; }
#ifdef FF8_PLAY
                    control_live_voice(s,n,1); REG(s,n*16+4) = saved; previous = 0;
#else
                    control_voice(s,n,1); REG(s,n*16+4) = saved; previous = 0;
#endif
                }
            }
        } else {
#ifdef FF8_PLAY
            for (n = 0; n < 24; ++n) control_live_voice(s,n,count);
#else
            for (n = 0; n < 24; ++n) control_voice(s,n,count);
#endif
            if (irq) control_aux(s);
            else control_aux_span(s,count);
            s->next_sample += (count - 1) * SPU_SAMPLE_CYCLES;
        }
        for (ch = 0; ch < 2; ++ch) {
            uint32_t work = s->volume_work[ch];
            for (n = 0; work; ++n, work >>= 1) if (work & 1)
                volume_advance(&s->voices[n].volume[ch],REG(s,n*16+ch*2),s->volume_sweep[ch] & (1u << n) ? count : 1);
        }
        for (ch = 0; ch < 2; ++ch) if (s->master_work & (1u << ch)) volume_advance(&s->master[ch],REG(s,0x180+ch*2),s->master_sweep & (1u << ch) ? count : 1);
        s->volume_work[0] = s->volume_sweep[0]; s->volume_work[1] = s->volume_sweep[1]; s->master_work = s->master_sweep;
        s->next_sample += SPU_SAMPLE_CYCLES;
    }
#ifdef FF8_PLAY
    if (s->next_sample > first_sample) {
        uint64_t advanced = (s->next_sample - first_sample) / SPU_SAMPLE_CYCLES;
        control_background(s, advanced > 735 ? 1470 : (unsigned)advanced * 2u);
    }
#endif
}
void spu_control_summary(const spu_t *s, FILE *out) {
    if (s->backend && !s->replay) {
        const spu_control_t *c = s->control;
        fprintf(out, "SPU control: syncs=%llu journal-full=%llu alias-spans=%llu alias-samples=%llu fifo-spans=%llu fifo-samples=%llu pmon-samples=%llu replay-samples=%llu allocation-fallback-calls=%llu allocation-fallback-samples=%llu deferred-bytes=%u journal-capacity=%u sync-journal=%llu sync-alias=%llu sync-fifo=%llu sync-read=%llu sync-pmon=%llu sync-explicit=%llu replay-voice-samples=%llu replay-alias-spans=%llu replay-fifo-spans=%llu\n",
            (unsigned long long)(c ? c->syncs : 0), (unsigned long long)(c ? c->journal_full : 0),
            (unsigned long long)(c ? c->aliases : 0), (unsigned long long)(c ? c->alias_frames : 0),
            (unsigned long long)(c ? c->fifo_spans : 0), (unsigned long long)(c ? c->fifo_frames : 0),
            (unsigned long long)(c ? c->pmon_frames : 0), (unsigned long long)(c ? c->replay_frames : 0),
            (unsigned long long)s->allocation_fallback_calls, (unsigned long long)s->allocation_fallback_frames, c ? c->used : 0, c ? c->capacity : 0,
            (unsigned long long)(c ? c->reasons[1] : 0), (unsigned long long)(c ? c->reasons[2] : 0),
            (unsigned long long)(c ? c->reasons[3] : 0), (unsigned long long)(c ? c->reasons[4] : 0),
            (unsigned long long)(c ? c->reasons[5] : 0), (unsigned long long)(c ? c->reasons[6] : 0),
            (unsigned long long)(c ? c->replay_voice_frames : 0), (unsigned long long)(c ? c->replay_alias_spans : 0),
            (unsigned long long)(c ? c->replay_fifo_spans : 0));
#ifdef FF8_PLAY
        fprintf(out, "SPU voice observers: reads=%llu samples=%llu fallbacks=%llu workspace=%u safe=%u partial=%u max-samples=%llu max-sync-samples=%llu pcm-voice-samples=%llu live-block-fetches=%llu\n",
            (unsigned long long)(c ? c->voice_reads : 0),
            (unsigned long long)(c ? c->voice_samples : 0),
            (unsigned long long)(c ? c->voice_fallbacks : 0),
            c && c->voice_shadow ? 1u : 0u, c ? c->voice_safe : 0, c ? c->partial_voice : 0,
            (unsigned long long)(c ? c->voice_max_samples : 0),
            (unsigned long long)(c ? c->sync_max_samples : 0),
            (unsigned long long)(c ? c->voice_pcm_frames : 0),
            (unsigned long long)s->live_block_fetches);
        fprintf(out, "SPU background: calls=%llu samples=%llu syncs=%llu pending-bytes=%u max-samples=%u max-bytes=%u\n",
            (unsigned long long)(c ? c->background_calls : 0),
            (unsigned long long)(c ? c->background_frames : 0),
            (unsigned long long)(c ? c->background_syncs : 0), c ? c->used - c->head : 0, c ? c->background_max_samples : 0, c ? c->background_max_bytes : 0);
#endif
    }
}
