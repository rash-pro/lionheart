#include "../runtime/spu.c"
#include <assert.h>
#include <stdio.h>
typedef struct { spu_t *s; uint64_t cycles[32]; unsigned count; } irq_log_t;
static void irq(void *ctx) { irq_log_t *log = ctx; assert(log->count < 32); log->cycles[log->count++] = spu_next_event(log->s); }
static const spu_backend_t sink = {NULL, NULL};
static void equal(spu_t *a, spu_t *b) {
    unsigned offset;
    for (offset = 0; offset < 0x400; offset += 2) {
        if (spu_read(a, offset) != spu_read(b, offset)) {
            fprintf(stderr,"read %x: %x != %x sample=%llu\n", offset, spu_read(a,offset), spu_read(b,offset), (unsigned long long)a->next_sample); abort();
        }
    }
    assert(spu_batch_deadline(a) == spu_batch_deadline(b));
    assert(spu_irq_deadline(a) == spu_irq_deadline(b));
    /* Deferred PCM histories may differ; the live control clocks may not. */
    for (offset=0;offset<24;++offset) {
        const voice_t *x=&a->voices[offset],*y=&b->voices[offset];
        if (memcmp(&x->adsr,&y->adsr,sizeof(x->adsr)) || memcmp(x->volume,y->volume,sizeof(x->volume)) ||
            x->phase!=y->phase || x->delay!=y->delay || x->position!=y->position || x->fraction!=y->fraction ||
            x->decoded!=y->decoded || x->flags!=y->flags || x->address!=y->address) {
            fprintf(stderr,"live voice %u position=%u/%u fraction=%u/%u phase=%u/%u\n",offset,x->position,y->position,x->fraction,y->fraction,x->phase,y->phase); abort();
        }
    }
#ifdef FF8_PLAY
    /* Every potentially audible history precedes the observer. An OFF/zero
     * history is restored by a nonzero ENVX write or reset by KON. */
    for (offset=0;offset<24;++offset) {
        if (!b->control || (b->control->live_pcm_valid & (1u << offset)))
            assert(!memcmp(&a->voices[offset],&b->voices[offset],sizeof(voice_t)));
        else assert(b->voices[offset].phase==ENV_OFF && !b->voices[offset].adsr.level);
    }
#endif
    assert(!memcmp(a->master,b->master,sizeof(a->master)));
    assert(a->capture==b->capture && a->reverb_address==b->reverb_address && a->fir_position==b->fir_position);
    assert(a->transfer==b->transfer && a->endx==b->endx && a->status==b->status && a->next_sample==b->next_sample);
    spu_sync(b);
    assert(!memcmp(a->ram, b->ram, sizeof(a->ram)));
    assert(!memcmp(a->voices, b->voices, sizeof(a->voices)));
    assert(!memcmp(a->reverb_input, b->reverb_input, sizeof(a->reverb_input)));
    assert(!memcmp(a->master, b->master, sizeof(a->master)));
    assert(a->noise == b->noise && a->noise_timer == b->noise_timer);
    if(a->blocks!=b->blocks || a->key_ons!=b->key_ons || a->frames!=b->frames)
        fprintf(stderr,"counters frames=%llu/%llu blocks=%llu/%llu key-ons=%llu/%llu\n",
            (unsigned long long)a->frames,(unsigned long long)b->frames,
            (unsigned long long)a->blocks,(unsigned long long)b->blocks,
            (unsigned long long)a->key_ons,(unsigned long long)b->key_ons);
    assert(a->blocks == b->blocks && a->key_ons == b->key_ons && a->frames == b->frames);
    assert(a->transfer == b->transfer && a->reverb_address == b->reverb_address);
    assert(a->next_sample == b->next_sample && a->capture == b->capture);
    assert(spu_batch_deadline(a) == spu_batch_deadline(b));
    assert(spu_irq_deadline(a) == spu_irq_deadline(b));
    assert(spu_dma_irq_words(a) == spu_dma_irq_words(b));
}
static void irqs_equal(irq_log_t *a, irq_log_t *b) {
    assert(a->count == b->count && !memcmp(a->cycles, b->cycles, a->count * sizeof(uint64_t)));
    a->count = b->count = 0;
}
static uint32_t random_word(uint32_t *seed) { *seed = *seed * 1664525u + 1013904223u; return *seed; }
static unsigned event_offsets[16], event_count;
static void event_write(void *ctx, unsigned offset, uint16_t value) {
    (void)ctx; (void)value; assert(event_count < 16); event_offsets[event_count++] = offset;
}
static void key_order(void) {
    static const spu_backend_t observer = {event_write, NULL};
    spu_t *s = spu_create();
    spu_set_backend(s, &observer, NULL);
    spu_write(s, 0x1aa, 0xc000); event_count = 0;
    spu_write(s, 0x188, 1); spu_write(s, 0x18c, 1); spu_write(s, 0x18a, 0x80);
    assert(event_count == 0);
    spu_advance(s, 768);
    assert(event_count == 3 && event_offsets[0] == 0x18c && event_offsets[1] == 0x188 && event_offsets[2] == 0x18a);
    spu_destroy(s);
}
static void envelope_batches(void) {
    uint32_t seed = 93;
    unsigned n, i;
    for (n = 0; n < 40000; ++n) {
        voice_t a = {0}, b;
        uint16_t low = (uint16_t)random_word(&seed), high = (uint16_t)random_word(&seed);
        uint16_t volume = (uint16_t)random_word(&seed);
        unsigned count = random_word(&seed) % 4097;
        a.phase = 1 + random_word(&seed) % 4;
        a.adsr.level = (int16_t)random_word(&seed); a.adsr.counter = random_word(&seed) & 0x7fff;
        b = a;
        for (i = 0; i < count; ++i) adsr_tick(&a,low,high);
        adsr_advance(&b,low,high,count);
        if (a.phase != b.phase || memcmp(&a.adsr,&b.adsr,sizeof(a.adsr))) {
            fprintf(stderr,"ADSR batch n=%u low=%04x high=%04x count=%u full=%u/%d/%u bulk=%u/%d/%u\n",n,low,high,count,a.phase,a.adsr.level,a.adsr.counter,b.phase,b.adsr.level,b.adsr.counter); abort();
        }
        a.adsr.level = (int16_t)random_word(&seed); a.adsr.counter = random_word(&seed) & 0x7fff; b = a;
        for (i = 0; i < count; ++i) volume_tick(&a.adsr,volume);
        volume_advance(&b.adsr,volume,count);
        assert(!memcmp(&a.adsr,&b.adsr,sizeof(a.adsr)));
    }
    puts("PASS bulk envelopes: 40000 ADSR and sweep rate/level/counter spans");
}
static void cached_deadlines(void) {
    spu_t *s = spu_create();
    unsigned sample, n;
    uint32_t seed = 83;
    spu_write(s, 0x1aa, 0xc040); spu_write(s, 0x1a4, 0x421);
    for (sample = 0; sample < 12000; ++sample) {
        uint64_t due[2] = {s->next_sample + 183 * 768, s->next_sample + 183 * 768};
        if (sample % 97 == 0) {
            n = random_word(&seed) % 24;
            spu_write(s, n * 16 + 4, (uint16_t)random_word(&seed));
            spu_write(s, n * 16 + 6, (uint16_t)random_word(&seed));
            spu_write(s, n * 16 + 14, (uint16_t)random_word(&seed));
            spu_write(s, n < 16 ? 0x188 : 0x18a, (uint16_t)(1u << (n % 16)));
            spu_write(s, 0x190, sample % 2 ? 0 : 2);
        }
        /* Recalculate the original horizon from live voice state, regardless
         * of cache age. Keep both conservative scheduler values unchanged. */
        for (n = 0; n < 24; ++n) {
            const voice_t *v = &s->voices[n];
            unsigned step = REG(s,n*16+4), delay;
            uint32_t next;
            uint64_t first;
            if (n && (flags(s,0x190) & (1u << n))) step = 0x4000;
            if (step > 0x4000) step = 0x4000;
            if (!v->decoded) delay = v->delay;
            else if (!step) continue;
            else delay = v->delay + ((28-v->position)*4096-v->fraction+step-1)/step;
            first = s->next_sample + delay * 768;
            if (first < due[0]) due[0] = first;
            next = !v->decoded ? v->address : v->flags & 1 ? REG(s,n*16+14)*8u & ~15u : (v->address+16) & RAM_MASK;
            if (((REG(s,0x1a4)*8u-next) & RAM_MASK) >= 16) first += 7*768;
            if (first < due[1]) due[1] = first;
        }
        if (s->key_on) due[0] = due[1] = s->next_sample;
        assert(spu_batch_deadline(s) == due[0]);
        assert(spu_irq_deadline(s) == due[1]);
        spu_advance(s,s->next_sample);
    }
    spu_destroy(s);
    puts("PASS cached horizons: 12000 original-model checkpoints, key latches, pitch/repeat changes and PMON");
}
static void loop_spans(void) {
    static const unsigned steps[] = {0,1,0x1000,0x3fff,0x4000,0xffff};
    static const unsigned counts[] = {1,2,7,8,28,29,183,4000};
    unsigned i,j,odd,enabled;
    for (i=0;i<6;++i) for(j=0;j<8;++j) for(odd=0;odd<2;++odd) for(enabled=0;enabled<2;++enabled) {
        spu_t *a=spu_create(),*b=malloc(sizeof(*b));
        irq_log_t logs[2]={{a,{0},0},{b,{0},0}};
        assert(b);
        a->ram[0x10001]=7; REG(a,4)=steps[i]; REG(a,14)=0x2000+odd; REG(a,0x1a4)=0x2000; REG(a,0x1aa)=enabled ? 0xc040 : 0xc000;
        a->voices[0].address=0x10000; a->voices[0].decoded=1; a->voices[0].flags=7;
        a->voices[0].position=27; a->voices[0].fraction=4095;
        memcpy(b,a,sizeof(*b)); spu_set_backend(b,&sink,NULL); spu_set_irq(a,irq,logs); spu_set_irq(b,irq,logs+1);
        spu_advance(a,counts[j]*768); spu_advance(b,counts[j]*768);
        irqs_equal(logs,logs+1); equal(a,b); spu_destroy(a); spu_destroy(b);
    }
    puts("PASS single-block loops: pitch limits, odd repeat writes, terminal fetches and exact IRQ cycles");
}
static void partial_reverb_alias(void) {
    spu_t *a=spu_create(),*b=malloc(sizeof(*b));
    unsigned n;
    assert(b);
    for(n=0;n<24;++n) { a->voices[n].decoded=1; a->voices[n].address=0x1000; }
    REG(a,0x1aa)=0xc080; REG(a,0x1ac)=4; REG(a,0x1a2)=0x2001; REG(a,0x190)=2;
    a->reverb_address=0x10008;
    a->voices[0].address=0x10000; a->voices[0].decoded=0; a->voices[0].phase=ENV_SUSTAIN; a->voices[0].adsr.level=32767;
    REG(a,4)=0x1000; REG(a,10)=0x1fc0; REG(a,14)=0x2000;
    memset(a->ram+0x10000,0x77,16); a->ram[0x10000]=0; a->ram[0x10001]=7;
    a->voices[1].address=0x2000; a->voices[1].decoded=0; a->voices[1].phase=ENV_SUSTAIN; a->voices[1].adsr.level=32767;
    REG(a,0x14)=0x1000; REG(a,0x1a)=0x1fc0; REG(a,0x1e)=0x400;
    a->ram[0x2081]=1;
    memcpy(b,a,sizeof(*b)); spu_set_backend(b,&sink,NULL);
    spu_advance(a,160*768); spu_advance(b,160*768);
    equal(a,b); assert(b->control->aliases && b->control->alias_frames==160);
    spu_destroy(a); spu_destroy(b);
    puts("PASS PMON fetch overlapping only the upper half of a reverb block");
}
static void output_spans(void) {
    spu_t *a = spu_create(), *b = spu_create();
    int16_t input[4000], left[2048], right[2048];
    unsigned n, i;
    uint32_t seed = 45;
    a->replay = 1; spu_set_backend(a,&sink,NULL); spu_set_backend(b,&sink,NULL);
    spu_set_backend(b,NULL,NULL); assert(!b->control); spu_set_backend(b,&sink,NULL); assert(b->control);
    for (n = 0; n < 300; ++n) {
        unsigned count = random_word(&seed) % 2001, got;
        for (i = 0; i < count*2; ++i) input[i] = (int16_t)random_word(&seed);
        spu_cd_input(a,input,count); spu_cd_input(b,input,count);
        spu_advance(a,a->next_sample + (random_word(&seed) % 2001)*768);
        spu_advance(b,a->next_sample - 768);
        assert(a->output_count == b->output_count && a->dropped == b->dropped);
        assert(a->cd_read == b->cd_read && a->cd_count == b->cd_count);
        if (n % 11) {
            got = spu_read_frames(a,left,1024);
            assert(got == spu_read_frames(b,right,1024)); assert(!memcmp(left,right,got*4));
        }
    }
    equal(a,b); spu_destroy(a); spu_destroy(b);
    puts("PASS raw CD output spans, FIFO wrapping, padding, overruns and startup backend replacement");
}
static void lazy_cases(void) {
    spu_t *a = spu_create(), *b = spu_create();
    irq_log_t logs[2] = {{a,{0},0},{b,{0},0}};
    unsigned n, i;
    int16_t *cd = calloc(20000 * 2, sizeof(*cd));
    assert(cd);
    spu_set_backend(b,&sink,NULL); spu_set_irq(a,irq,logs); spu_set_irq(b,irq,logs+1);
    for (n = 0; n < 24; ++n) {
        unsigned offsets[] = {n*16+6,n*16+4,n*16+8,n*16+10};
        unsigned values[] = {0x2000,0x1000,0x000f,0x0000};
        for (i = 0; i < 4; ++i) { spu_write(a,offsets[i],values[i]); spu_write(b,offsets[i],values[i]); }
    }
    spu_write(a,0x1a6,0x2000); spu_write(b,0x1a6,0x2000);
    spu_dma_write(a,0x11110700); spu_dma_write(b,0x11110700);
    spu_write(a,0x1a2,0x6000); spu_write(b,0x1a2,0x6000);
    spu_write(a,0x1aa,0xc080); spu_write(b,0x1aa,0xc080);
    spu_write(a,0x1ac,4); spu_write(b,0x1ac,4);
    spu_write(a,0x188,0xffff); spu_write(b,0x188,0xffff);
    spu_write(a,0x18a,0xff); spu_write(b,0x18a,0xff);
    spu_advance(a,2048*768); spu_advance(b,2048*768);
    assert(b->control->shadow->frames == 0 && b->interpolations == 0 && b->bulk_frames == 2048);
    /* Capture IRQ clocks run while their PCM values are still deferred. */
    spu_write(a,0x1a4,0x100); spu_write(b,0x1a4,0x100);
    spu_write(a,0x1aa,0xc0c0); spu_write(b,0x1aa,0xc0c0);
    spu_advance(a,2600*768); spu_advance(b,2600*768); irqs_equal(logs,logs+1);
    assert(b->control->shadow->frames == 0);
    /* DMA observes deferred reverb work RAM, then capture; both must be exact. */
    spu_write(a,0x1a6,0x6000); spu_write(b,0x1a6,0x6000);
    assert(spu_dma_read(a) == spu_dma_read(b)); equal(a,b);
    spu_write(a,0x1a6,0x100); spu_write(b,0x1a6,0x100);
    assert(spu_dma_read(a) == spu_dma_read(b)); equal(a,b);
    /* A voice fetching capture, then reverb, must use the exact fallback. */
    for (i = 0; i < 2; ++i) {
        spu_write(a,0x56,i ? 0x6000 : 0x100); spu_write(b,0x56,i ? 0x6000 : 0x100);
        spu_write(a,0x188,0x20); spu_write(b,0x188,0x20);
        spu_advance(a,a->next_sample + 768*300); spu_advance(b,b->next_sample + 768*300);
        irqs_equal(logs,logs+1); equal(a,b);
    }
    assert(b->control->aliases && b->control->alias_frames);
    /* PMON dependency changes materialize history before the new modulator. */
    spu_write(a,0x190,4); spu_write(b,0x190,4);
    spu_advance(a,a->next_sample+768*80); spu_advance(b,b->next_sample+768*80);
    irqs_equal(logs,logs+1); equal(a,b);
    /* Force bounded-journal replay and oversized public CD submission. */
    for (i = 0; i < 65; ++i) { spu_cd_input(a,cd,20000); spu_cd_input(b,cd,20000); }
    assert(b->control->journal_full); equal(a,b);
    assert(a->cd_count == b->cd_count && a->cd_received == b->cd_received && a->cd_dropped == b->cd_dropped);
    control_destroy(b); b->control = NULL;
    spu_advance(a,a->next_sample+768*20); spu_advance(b,b->next_sample+768*20);
    irqs_equal(logs,logs+1); equal(a,b); assert(b->allocation_fallback_frames);
    free(cd); spu_destroy(a); spu_destroy(b);
    puts("PASS lazy capture/reverb reads, IRQs, voice aliases, PMON history, journal limit and allocation fallback");
}
static void directed(void) {
    spu_t *a = spu_create(), *b = spu_create();
    irq_log_t logs[2] = {{a,{0},0},{b,{0},0}};
    uint32_t seed = 8;
    unsigned n, mode;
    spu_set_backend(b, &sink, NULL); spu_set_irq(a, irq, logs); spu_set_irq(b, irq, logs + 1);
    for (n = 0; n < 0x80000; n += 4) {
        uint32_t data = random_word(&seed);
        /* Include loop/end flags and invalid ADPCM headers in arbitrary RAM. */
        spu_dma_write(a,data); spu_dma_write(b,data);
    }
    for (mode = 0; mode < 8; ++mode) {
        unsigned r;
        for (r = 0; r < 0x260; r += 2) {
            uint16_t value = (uint16_t)random_word(&seed);
            if (r == 0x190 || r == 0x192 || r == 0x198 || r == 0x19a || r == 0x1ac) value = 0;
            spu_write(a,r,value); spu_write(b,r,value);
        }
        for (n = 0; n < 1500; ++n) {
            uint64_t until = a->next_sample + (random_word(&seed) % 401) * 768;
            unsigned offset = random_word(&seed) % 0x130 * 2;
            uint16_t value = (uint16_t)random_word(&seed);
            int16_t cd[80];
            if (n % 7 == 0) {
                for (r = 0; r < 80; ++r) cd[r] = (int16_t)random_word(&seed);
                spu_cd_input(a,cd,40); spu_cd_input(b,cd,40);
            }
            if (offset == 0x190 || offset == 0x192 || offset == 0x198 || offset == 0x19a || offset == 0x1ac) offset = 0x188;
            spu_write(a,offset,value); spu_write(b,offset,value);
            if (!(n % 13)) {
                unsigned controls[] = {0xc000,0xc0c0,0xc080,0xc040,0x40,0xc010,0xc020,0xc030};
                spu_write(a,0x1aa,controls[mode]); spu_write(b,0x1aa,controls[mode]);
                spu_write(a,0x1ac,mode ? 4 : 0); spu_write(b,0x1ac,mode ? 4 : 0);
                spu_write(a,0x190,mode >= 2 ? (uint16_t)random_word(&seed) : 0); spu_write(b,0x190,REG(a,0x190));
                spu_write(a,0x198,mode >= 3 ? (uint16_t)random_word(&seed) : 0); spu_write(b,0x198,REG(a,0x198));
            }
            if (!(n % 19)) { uint32_t data = random_word(&seed); spu_dma_write(a,data); spu_dma_write(b,data); }
            if (!(n % 23)) assert(spu_dma_read(a) == spu_dma_read(b));
            spu_advance(a,until); spu_advance(b,until);
            irqs_equal(logs,logs+1); equal(a,b);
        }
    }
    assert(b->bulk_frames); printf("PASS control directed frames=%llu bulk=%llu\n", (unsigned long long)b->frames, (unsigned long long)b->bulk_frames);
    spu_destroy(a); spu_destroy(b);
}
static void replay(const char *path) {
    FILE *file = fopen(path,"rb"); char magic[8]; uint32_t row[3];
    spu_t *a = spu_create(), *b = spu_create(); uint64_t records = 0, reads = 0;
    irq_log_t logs[2] = {{a,{0},0},{b,{0},0}};
    assert(file && fread(magic,8,1,file) == 1 && !memcmp(magic,"FF8SPUT1",8));
    spu_set_backend(b,&sink,NULL); spu_set_irq(a,irq,logs); spu_set_irq(b,irq,logs+1);
    for (;;) {
        size_t bytes = fread(row,1,sizeof(row),file);
        if (!bytes) break;
        if (bytes != sizeof(row)) { fputs("incomplete SPU trace record\n",stderr); exit(2); }
        switch (row[0]) {
        case 1: spu_advance(a,row[1] | ((uint64_t)row[2] << 32)); spu_advance(b,row[1] | ((uint64_t)row[2] << 32)); break;
        case 2: assert(spu_read(a,row[1]) == row[2]); assert(spu_read(b,row[1]) == row[2]); ++reads; break;
        case 3: spu_write(a,row[1],row[2]); spu_write(b,row[1],row[2]); break;
        case 4: assert(spu_dma_read(a) == row[2]); assert(spu_dma_read(b) == row[2]); ++reads; break;
        case 5: spu_dma_write(a,row[2]); spu_dma_write(b,row[2]); break;
        case 6: {
            int16_t *pcm = malloc(row[1] * 4u); assert(pcm && fread(pcm,4,row[1],file) == row[1]);
            spu_cd_input(a,pcm,row[1]); spu_cd_input(b,pcm,row[1]); free(pcm); break;
        }
        default: abort();
        }
        irqs_equal(logs,logs+1);
        if (!(++records % 65536)) equal(a,b);
        /* Drain output to bound unrelated output overrun counters. */
        if (a->output_count > 8192) { a->output_read = a->output_count = 0; b->output_read = b->output_count = 0; }
    }
    assert(!ferror(file));
    if (!records || !reads || !b->frames) { fputs("empty SPU gameplay trace\n",stderr); exit(2); }
    equal(a,b); fclose(file);
    printf("PASS replay records=%llu reads=%llu frames=%llu bulk=%llu full_interpolations=%llu control_interpolations=%llu\n",
        (unsigned long long)records,(unsigned long long)reads,(unsigned long long)b->frames,(unsigned long long)b->bulk_frames,
        (unsigned long long)a->interpolations,(unsigned long long)b->interpolations);
    spu_destroy(a); spu_destroy(b);
}
int main(int argc, char **argv) { if (argc == 2) replay(argv[1]); else { key_order(); envelope_batches(); cached_deadlines(); loop_spans(); partial_reverb_alias(); output_spans(); lazy_cases(); directed(); } return 0; }
