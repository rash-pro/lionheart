/* Lazy control-state observers are compared with the complete scalar SPU. */
#include "../runtime/spu.c"
#include <assert.h>
#include <stdio.h>
static const spu_backend_t sink = {NULL, NULL};
static void pair_write(spu_t *a, spu_t *b, unsigned offset, unsigned value) {
    spu_write(a, offset, (uint16_t)value); spu_write(b, offset, (uint16_t)value);
}
static void setup(spu_t *a, spu_t *b) {
    unsigned n;
    a->ram[0x2001] = 7;
    memset(a->ram + 0x2002, 0x77, 14);
    memcpy(b, a, sizeof(*b)); a->replay = 1;
    spu_set_backend(a, &sink, NULL); spu_set_backend(b, &sink, NULL);
    spu_advance(a, SPU_SAMPLE_CYCLES); spu_advance(b, SPU_SAMPLE_CYCLES);
    pair_write(a, b, 0x1aa, 0xfc80);
    pair_write(a, b, 0x1ac, 4); pair_write(a, b, 0x1a2, 0xe000);
    for (n = 0; n < 2; ++n) {
        pair_write(a, b, n*16+4, 0x1000); pair_write(a, b, n*16+6, 0x400);
        pair_write(a, b, n*16+14, 0x400); pair_write(a, b, n*16+8, 0xf);
        pair_write(a, b, n*16+10, 0x1fc0);
    }
    pair_write(a, b, 0x188, 3);
}
static void advance_pair(spu_t *a, spu_t *b, unsigned count) {
    spu_advance(a, a->next_sample + (count-1u)*SPU_SAMPLE_CYCLES);
    spu_advance(b, b->next_sample + (count-1u)*SPU_SAMPLE_CYCLES);
}
static void no_observers(void) {
    spu_t *s = spu_create(); unsigned n;
    assert(s); spu_set_backend(s, &sink, NULL); spu_write(s, 0x1aa, 0xc000);
    for (n = 0; n < 1000; ++n) spu_advance(s, s->next_sample + 63*SPU_SAMPLE_CYCLES);
    assert(!s->control->replay_frames && !s->control->background_calls);
    spu_destroy(s); puts("PASS unobserved sparse history does no canonical synthesis");
}
static void clean_and_dirty_reads(void) {
    spu_t *a=spu_create(), *b=spu_create(); uint64_t before;
    assert(a && b); setup(a,b); advance_pair(a,b,12000);
    assert((b->control->loops&3u)==3u);
    before=b->control->replay_frames;
    pair_write(a,b,0x1a6,0x400); /* Outside all deferred work areas. */
    assert(spu_dma_read(a)==spu_dma_read(b));
    assert(b->control->replay_frames==before && b->control->voice_safe);
    pair_write(a,b,0x1a6,0); /* Capture is a real RAM observer. */
    assert(spu_dma_read(a)==spu_dma_read(b));
    assert(b->control->replay_frames>before && b->control->reasons[4]==1);
    assert(!memcmp(spu_ram(a),spu_ram(b),512*1024));
    spu_destroy(a); spu_destroy(b); puts("PASS clean transfer reads stay live; capture reads materialize");
}
static void resolved_pitch(void) {
    spu_t *a=spu_create(), *b=spu_create(); unsigned n; uint64_t full, reads;
    assert(a && b); setup(a,b); pair_write(a,b,0x190,2); advance_pair(a,b,12000);
    assert(spu_read(a,0x19c)==spu_read(b,0x19c));
    assert(!b->control->voice_reads);
    assert(!b->control->pending_pitch);
    full=b->control->replay_frames; reads=b->control->voice_reads;
    for(n=0;n<100;++n) {
        advance_pair(a,b,64);
        assert(!b->control->pending_pitch);
        assert(spu_read(a,0x19c)==spu_read(b,0x19c));
        assert(spu_read(a,0x1c)==spu_read(b,0x1c));
        assert(a->voices[1].position==b->voices[1].position);
        assert(a->voices[1].fraction==b->voices[1].fraction);
    }
    assert(b->control->replay_frames==full && b->control->voice_reads==reads);
    pair_write(a,b,0x194,1); /* Newly consumed noise must restore its clock. */
    advance_pair(a,b,333);
    assert(spu_read(a,0x19c)==spu_read(b,0x19c));
    assert(a->voices[1].position==b->voices[1].position);
    assert(a->voices[1].fraction==b->voices[1].fraction);
    assert(b->control->replay_frames==full);
    pair_write(a,b,0x190,0); advance_pair(a,b,64);
    assert(!b->control->pending_pitch);
    assert(spu_read(a,0x19c)==spu_read(b,0x19c));
    assert(!memcmp(spu_ram(a),spu_ram(b),512*1024));
    spu_destroy(a); spu_destroy(b); puts("PASS resolved PMON stays current without re-arming on every advance");
}
static void noise_clocks(void) {
    static const unsigned counts[]={1,2,7,183,4096};
    spu_t *a=spu_create(),*b=spu_create(); unsigned shift,step,start,n;
    assert(a&&b);a->replay=1;spu_set_backend(a,&sink,NULL);spu_set_backend(b,&sink,NULL);
    for(shift=0;shift<16;++shift) for(step=0;step<4;++step) for(start=0;start<4;++start) {
        pair_write(a,b,0x1aa,0x8000u|(shift<<10)|(step<<8));
        for(n=0;n<sizeof(counts)/sizeof(counts[0]);++n) {
            a->noise=b->noise=(uint16_t)(n*1231u+shift*71u+step);
            a->noise_timer=b->noise_timer=start==0?0:start==1?1:start==2?65536:131071;
            advance_pair(a,b,counts[n]);
            assert(a->noise==b->noise && a->noise_timer==b->noise_timer);
        }
    }
    spu_destroy(a);spu_destroy(b);puts("PASS analytic noise clocks across every rate, step, inherited timer and span boundary");
}
typedef struct {spu_t *s;unsigned count;uint64_t sample;} irq_record_t;
static void record_irq(void *context) {
    irq_record_t *r=context;++r->count;r->sample=spu_next_event(r->s);
}
static void irq_without_ram(void) {
    spu_t *a=spu_create(),*b=spu_create();uint64_t before;
    irq_record_t left={a,0,0},right={b,0,0};
    assert(a&&b);setup(a,b);pair_write(a,b,0x190,2);advance_pair(a,b,12000);
    assert(spu_read(a,0x19c)==spu_read(b,0x19c));assert(!b->control->partial_voice);
    spu_set_irq(a,record_irq,&left);spu_set_irq(b,record_irq,&right);
    before=b->control->replay_frames;pair_write(a,b,0x1a4,0);
    pair_write(a,b,0x1aa,0xfcc0);
    assert(b->control->replay_frames==before);
    advance_pair(a,b,4096);
    assert(left.count==1 && right.count==1 && left.sample==right.sample);
    assert(spu_read(a,0x1ae)==spu_read(b,0x1ae));
    assert(spu_read(a,0x19c)==spu_read(b,0x19c));
    assert(a->voices[1].position==b->voices[1].position && a->voices[1].fraction==b->voices[1].fraction);
    assert(b->control->replay_frames==before);
    assert(!memcmp(spu_ram(a),spu_ram(b),512*1024));
    spu_destroy(a);spu_destroy(b);puts("PASS IRQ after resolved metadata retains exact cycle/address hits without RAM synthesis");
}
static void historical_irq_and_noise(void) {
    unsigned kind,n;
    for(kind=0;kind<3;++kind) {
        spu_t *a=spu_create(),*b=spu_create();uint64_t before;
        irq_record_t left={a,0,0},right={b,0,0};
        assert(a&&b);setup(a,b);
        spu_set_irq(a,record_irq,&left);spu_set_irq(b,record_irq,&right);
        if(kind!=0) {
            pair_write(a,b,0x1a4,0);pair_write(a,b,0x1aa,0xfcc0);
            advance_pair(a,b,1024);pair_write(a,b,0x1aa,0xfc80);
        }
        for(n=22;n<24;++n) {
            pair_write(a,b,n*16+4,0x1000);pair_write(a,b,n*16+6,0x400);
            pair_write(a,b,n*16+14,0x400);pair_write(a,b,n*16+8,0xf);
            pair_write(a,b,n*16+10,0x1fc0);
        }
        if(kind!=1)pair_write(a,b,0x196,0x40);
        pair_write(a,b,0x192,0x80);pair_write(a,b,0x18a,0xc0);
        advance_pair(a,b,12000);before=b->control->replay_frames;
        assert(spu_read(a,0x17c)==spu_read(b,0x17c));
        assert(spu_read(a,0x19e)==spu_read(b,0x19e));
        assert(b->control->replay_frames==before && !b->control->voice_reads);
        assert(a->voices[23].position==b->voices[23].position && a->voices[23].fraction==b->voices[23].fraction);
        assert(a->noise==b->noise && a->noise_timer==b->noise_timer);
        assert(left.count==right.count && left.sample==right.sample);
        assert(!memcmp(spu_ram(a),spu_ram(b),512*1024));
        spu_destroy(a);spu_destroy(b);
    }
    puts("PASS first observations after historical IRQ/noise preserve high-voice PMON, RAM and IRQ cycles without canonical replay");
}
static void decoder_reset_history(void) {
    unsigned order;
    for(order=0;order<3;++order) {
        spu_t *a=spu_create(),*b=spu_create();uint64_t before;
        assert(a&&b);setup(a,b);advance_pair(a,b,64000);
        if(order==0)pair_write(a,b,0x188,1);
        if(order!=0)pair_write(a,b,0x190,2);
        if(order==2)advance_pair(a,b,100);
        if(order!=0)pair_write(a,b,0x188,1);
        if(order!=1)pair_write(a,b,0x190,2);
        advance_pair(a,b,333);before=b->control->replay_frames;
        assert(spu_read(a,0x1c)==spu_read(b,0x1c));
        assert(spu_read(a,0x19c)==spu_read(b,0x19c));
        assert(!memcmp(a->voices[0].history,b->voices[0].history,sizeof(a->voices[0].history)));
        assert(!memcmp(a->voices[0].samples,b->voices[0].samples,sizeof(a->voices[0].samples)));
        assert(!b->control->voice_pcm_frames && !b->control->voice_reads);
        assert(a->voices[1].position==b->voices[1].position && a->voices[1].fraction==b->voices[1].fraction);
        assert(b->control->replay_frames==before);
        pair_write(a,b,0x188,1);advance_pair(a,b,100);
        assert(spu_read(a,0x1c)==spu_read(b,0x1c));
        assert(!memcmp(spu_ram(a),spu_ram(b),512*1024));
        spu_destroy(a);spu_destroy(b);
    }
    puts("PASS current decoder history needs no observer replay, including same-clock KON/PMON order");
}
/* Zero ENVX while off cannot feed PMON. Only KON or a nonzero ENVX write
 * can make its discarded decoder history observable again. */
static void silent_history(void) {
    unsigned mode;
    for(mode=0;mode<4;++mode) {
        spu_t *a=spu_create(),*b=spu_create();unsigned i,n=mode<2?0:22;
        uint64_t fetches,before;
        irq_record_t left={a,0,0},right={b,0,0};
        assert(a&&b);setup(a,b);
        pair_write(a,b,0x190,0);pair_write(a,b,0x188,0);
        pair_write(a,b,0x18c,3);advance_pair(a,b,128);
        pair_write(a,b,0x1a6,0x800);
        for(i=0;i<8;++i) {
            uint32_t word=i==0?0x77770000u:i==4?0x11110320u:0x12345678u;
            spu_dma_write(a,word);spu_dma_write(b,word);
        }
        for(i=n;i<n+2;++i) {
            pair_write(a,b,i*16+4,0x1000);pair_write(a,b,i*16+6,0x800);
            pair_write(a,b,i*16+14,0x800);
        }
        pair_write(a,b,n<16?0x190:0x192,1u<<((n+1)%16));
        pair_write(a,b,n<16?0x188:0x18a,3u<<(n%16));advance_pair(a,b,64);
        pair_write(a,b,0x1aa,0x7c80);advance_pair(a,b,1);
        pair_write(a,b,0x1aa,0xfc80);
        /* Save a complete canonical starting point before the silent span. */
        spu_sync(b);fetches=b->live_block_fetches;before=b->control->replay_frames;
        advance_pair(a,b,64000);
        assert(spu_read(a,n*16+12)==spu_read(b,n*16+12));
        assert(spu_read(a,0x19c)==spu_read(b,0x19c));
        assert(b->control->replay_frames==before);
        assert(b->live_block_fetches-fetches<100);
        pair_write(a,b,n<16?0x188:0x18a,1u<<((n+1)%16));
        if(mode&1) pair_write(a,b,n<16?0x188:0x18a,1u<<(n%16));
        else pair_write(a,b,n*16+12,0x4000);
        spu_set_irq(a,record_irq,&left);spu_set_irq(b,record_irq,&right);
        pair_write(a,b,0x1a4,0);pair_write(a,b,0x1aa,0xfcc0);
        if(mode>=2)pair_write(a,b,0x196,1u<<(n%16));
        advance_pair(a,b,333);
        assert(left.count==right.count && left.sample==right.sample);
        assert(a->voices[n+1].position==b->voices[n+1].position);
        assert(a->voices[n+1].fraction==b->voices[n+1].fraction);
        assert(!memcmp(a->voices[n].history,b->voices[n].history,sizeof(a->voices[n].history)));
        assert(spu_read(a,n*16+12)==spu_read(b,n*16+12));
        assert(!memcmp(spu_ram(a),spu_ram(b),512*1024));
        spu_destroy(a);spu_destroy(b);
    }
    puts("PASS silent decoder elision: KON reset or writable ENVX restores observable PMON history");
}
/* Frequent streaming writes put the journal under pressure while the live
 * calendar advances only a few samples at a time. Earlier shadow progress may
 * not change later dirty-RAM observers, IRQs or published decoder history. */
static void streaming_pressure(void) {
    spu_t *a=spu_create(),*b=spu_create();unsigned i;
    assert(a&&b);setup(a,b);b->control->capacity=8192;
    pair_write(a,b,0x1a6,0x800);
    for(i=0;i<8000;++i) {
        uint32_t word=i*1234567u;
        spu_dma_write(a,word);spu_dma_write(b,word);
        advance_pair(a,b,5);
        if(i%617==616) {
            pair_write(a,b,0x1a6,0);
            assert(spu_dma_read(a)==spu_dma_read(b));
            pair_write(a,b,0x1a6,0x800);
        }
    }
    assert(b->control->background_calls && b->control->background_max_samples<=1470);
    assert(!memcmp(spu_ram(a),spu_ram(b),512*1024));
    assert(!memcmp(a->voices,b->voices,sizeof(a->voices)));
    assert(a->next_sample==b->next_sample && a->endx==b->endx);
    spu_destroy(a);spu_destroy(b);puts("PASS streaming journal pressure retains canonical RAM and voice state");
}
static void replay_budget(void) {
    spu_t *a=spu_create(),*b=spu_create();unsigned i;uint64_t before;
    assert(a&&b);setup(a,b);b->control->capacity=8192;
    advance_pair(a,b,10000);
    /* Make a sparse old history reach storage pressure without giving the
     * replay any live-clock budget. New tiny advances may spend only twice
     * their sample count, even though the old history is much longer. */
    for(i=0;i<400;++i)pair_write(a,b,0,(uint16_t)(i*71u));
    before=b->control->background_frames;
    for(i=0;i<2000;++i) {
        advance_pair(a,b,5);
        assert(b->control->background_frames-before<=2u*(i+1)*5u);
    }
    assert(b->control->background_calls);
    assert(!memcmp(spu_ram(a),spu_ram(b),512*1024));
    assert(!memcmp(a->voices,b->voices,sizeof(a->voices)));
    spu_destroy(a);spu_destroy(b);puts("PASS batched replay never borrows future live-clock budget");
}
int main(void) { replay_budget(); streaming_pressure(); silent_history(); no_observers(); clean_and_dirty_reads(); resolved_pitch(); noise_clocks(); irq_without_ram(); historical_irq_and_noise(); decoder_reset_history(); return 0; }
