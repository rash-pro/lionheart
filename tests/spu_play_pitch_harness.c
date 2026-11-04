/* Current voice histories and lazy capture/reverb RAM against scalar SPU. */
#include "../runtime/spu.c"
#include <assert.h>
#include <stdio.h>
static const spu_backend_t sink = {NULL,NULL};
typedef struct { spu_t *spu; unsigned count; uint64_t at[64]; } irq_log_t;
static void irq(void *p) { irq_log_t *l=p;assert(l->count<64);l->at[l->count++]=spu_next_event(l->spu); }
static uint32_t random_word(uint32_t *s) { *s=*s*1664525u+1013904223u;return *s; }
static void write_pair(spu_t *a,spu_t *b,unsigned r,unsigned v){spu_write(a,r,v);spu_write(b,r,v);}
static void observe(spu_t *a,spu_t *b) {
    unsigned r;
    for(r=0;r<0x400;r+=2)assert(spu_read(a,r)==spu_read(b,r));
    spu_sync(b);
    assert(!memcmp(a,b,offsetof(spu_t,output)));
    assert(a->next_sample==b->next_sample && a->frames==b->frames);
    assert(spu_batch_deadline(a)==spu_batch_deadline(b));
    assert(spu_irq_deadline(a)==spu_irq_deadline(b));
}
static void first_observers(void) {
    unsigned kind,n;
    for(kind=0;kind<3;++kind) {
        unsigned target=kind==1?17:1,source=target-1,offset;
        spu_t *a=spu_create(),*b=spu_create();
        assert(a&&b);
        memset(a->ram+0x2000,0x77,16);a->ram[0x2000]=0;a->ram[0x2001]=7;
        memset(a->ram+0x4000,0x77,48);a->ram[0x4000]=0;a->ram[0x4001]=kind==2?0:1;
        a->ram[0x4010]=0;a->ram[0x4011]=7;
        memcpy(b,a,sizeof(*b));a->replay=1;spu_set_backend(a,&sink,NULL);spu_set_backend(b,&sink,NULL);
        write_pair(a,b,0x1aa,0xc000);
        for(n=source;n<=target;++n) {
            write_pair(a,b,n*16+4,0x1000);write_pair(a,b,n*16+6,n==source?0x400:0x800);
            write_pair(a,b,n*16+14,n==source?0x400:0x800);
            write_pair(a,b,n*16+8,0xf);write_pair(a,b,n*16+10,0x1fc0);
        }
        write_pair(a,b,target<16?0x190:0x192,1u<<(target%16));
        write_pair(a,b,target<16?0x188:0x18a,3u<<(source%16));
        spu_advance(a,24*768);spu_advance(b,24*768);
        assert(!b->control->pending_pitch);
        /* Clearing PMON leaves already-current positions and repeat addresses. */
        write_pair(a,b,target<16?0x190:0x192,0);
        offset=kind==2?target*16+14:kind==1?0x19e:0x19c;
        if(kind==2)assert(REG(a,offset)==REG(b,offset));
        else assert(a->endx==b->endx);
        assert(spu_read(a,offset)==spu_read(b,offset));
        observe(a,b);spu_destroy(a);spu_destroy(b);
    }
    puts("PASS first observers: low/high ENDX and repeat address after PMON clears");
}
static void sparse_history_observer(void) {
    spu_t *a=spu_create(),*b=spu_create();unsigned i;uint64_t before;
    assert(a&&b);
    memset(a->ram+0x2000,0x77,16);a->ram[0x2000]=0;a->ram[0x2001]=7;
    memcpy(b,a,sizeof(*b));a->replay=1;
    spu_set_backend(a,&sink,NULL);spu_set_backend(b,&sink,NULL);
    write_pair(a,b,0x1aa,0xc000);write_pair(a,b,0x190,2);
    for(i=0;i<2;++i) {
        write_pair(a,b,i*16+4,0x1000);write_pair(a,b,i*16+6,0x400);
        write_pair(a,b,i*16+14,0x400);write_pair(a,b,i*16+8,0xf);
        write_pair(a,b,i*16+10,0x1fc0);
    }
    write_pair(a,b,0x188,3);
    /* Few register events never fill the journal, but a later ENDX read must
     * not replay seconds of accumulated pitch history in one guest step. */
    for(i=0;i<4000;++i) {
        spu_advance(a,a->next_sample+63*768);spu_advance(b,b->next_sample+63*768);
    }
    assert(b->control->used-b->control->head<b->control->capacity/2);
    before=b->control->replay_frames;
    assert(spu_read(a,0x19c)==spu_read(b,0x19c));
    assert(b->control->replay_frames-before<=4096+64);
    assert(!b->control->voice_reads && !b->control->partial_voice);
    assert(!b->control->journal_full);observe(a,b);
    spu_destroy(a);spu_destroy(b);
    puts("PASS sparse-history ENDX observer: exact state without a full-RAM catch-up burst");
}
static void repeated_voice_observers(void) {
    spu_t *a=spu_create(),*b=spu_create();unsigned i,n;
    assert(a&&b);
    memset(a->ram+0x2000,0x77,16);a->ram[0x2000]=0;a->ram[0x2001]=7;
    memcpy(b,a,sizeof(*b));a->replay=1;
    spu_set_backend(a,&sink,NULL);spu_set_backend(b,&sink,NULL);
    write_pair(a,b,0x1aa,0xc000);write_pair(a,b,0x190,2);
    for(n=0;n<3;++n) {
        write_pair(a,b,n*16+4,0x1000);write_pair(a,b,n*16+6,0x400);
        write_pair(a,b,n*16+14,0x400);write_pair(a,b,n*16+8,0xf);
        write_pair(a,b,n*16+10,0x1fc0);write_pair(a,b,n*16,0x8004);
    }
    write_pair(a,b,0x188,7);write_pair(a,b,0x1a6,0x8000);
    for(i=0;i<1200;++i) {
        if(i==400)write_pair(a,b,0x190,4);
        if(i==800)write_pair(a,b,0x190,6);
        spu_advance(a,a->next_sample+63*768);spu_advance(b,b->next_sample+63*768);
        assert(spu_read(a,0x19c)==spu_read(b,0x19c));
        assert(spu_read(a,0x2c)==spu_read(b,0x2c));
        for(n=0;n<3;++n)assert(spu_read(a,0x200+n*4)==spu_read(b,0x200+n*4));
        for(n=0;n<3;++n) {
            assert(a->voices[n].address==b->voices[n].address);
            assert(a->voices[n].position==b->voices[n].position);
            assert(a->voices[n].fraction==b->voices[n].fraction);
        }
        if(i%120==0) {
            write_pair(a,b,0x1a6,0x400);
            spu_dma_write(a,0x11110700u^((i&240u)<<16));
            spu_dma_write(b,0x11110700u^((i&240u)<<16));
            write_pair(a,b,0x1a6,0x8000);
        }
        /* A consumed voice-view event must not grow behind its cursor. */
        spu_dma_write(a,i);spu_dma_write(b,i);
        spu_dma_write(a,~i);spu_dma_write(b,~i);
    }
    /* Dependency changes and repeated polls consume current predecessor histories. */
    assert(!b->control->voice_reads);
    assert(b->control->voice_samples<UINT64_C(1200)*64*4);
    assert(!b->control->partial_voice);
    write_pair(a,b,0x190,0); /* no pending PMON after the last resolved read */
    assert(spu_read(a,0x19c)==spu_read(b,0x19c));
    assert(!b->control->pending_pitch);
    write_pair(a,b,0x1aa,0xc040); /* IRQ uses exact live metadata/address clocks. */
    assert(!b->control->partial_voice);observe(a,b);
    spu_destroy(a);spu_destroy(b);
    puts("PASS current voice observers: sweeps, PMON growth and IRQ clocks without history replay");
}
static void voice_then_ram_observer(void) {
    spu_t *a=spu_create(),*b=spu_create();unsigned i;uint64_t before;
    assert(a&&b);
    memset(a->ram+0x2000,0x77,16);a->ram[0x2000]=0;a->ram[0x2001]=7;
    memcpy(b,a,sizeof(*b));a->replay=1;
    spu_set_backend(a,&sink,NULL);spu_set_backend(b,&sink,NULL);
    spu_advance(a,768);spu_advance(b,768);
    write_pair(a,b,0x1aa,0xc080);write_pair(a,b,0x1ac,4);write_pair(a,b,0x1a2,0xf000);
    write_pair(a,b,0x190,2);
    for(i=0;i<2;++i) {
        write_pair(a,b,i*16+4,0x1000);write_pair(a,b,i*16+6,0x400);
        write_pair(a,b,i*16+14,0x400);write_pair(a,b,i*16+8,0xf);
        write_pair(a,b,i*16+10,0x1fc0);
    }
    write_pair(a,b,0x188,3);
    for(i=0;i<200;++i) {
        spu_advance(a,a->next_sample+63*768);spu_advance(b,b->next_sample+63*768);
    }
    assert(spu_read(a,0x19c)==spu_read(b,0x19c));
    assert(!b->control->voice_reads && !b->control->partial_voice);
    before=b->control->replay_frames;
    assert(!memcmp(spu_ram(a),spu_ram(b),512*1024));
    /* A full RAM observation explicitly pays the previously deferred history. */
    assert(b->control->replay_frames>before);
    assert(!b->control->partial_voice);observe(a,b);
    spu_destroy(a);spu_destroy(b);
    puts("PASS current voices followed by complete capture/reverb RAM observation");
}
static void voice_workspace_fallback(void) {
    unsigned kind,i;
    for(kind=0;kind<2;++kind) {
        spu_t *a=spu_create(),*b=spu_create();uint64_t before;
        assert(a&&b);a->ram[0x2001]=7;memcpy(b,a,sizeof(*b));a->replay=1;
        spu_set_backend(a,&sink,NULL);spu_set_backend(b,&sink,NULL);
        if(!kind){free(b->control->voice_shadow);b->control->voice_shadow=NULL;}
        write_pair(a,b,0x1aa,0xc000);write_pair(a,b,0x190,2);
        if(kind)write_pair(a,b,0x194,1);
        for(i=0;i<2;++i) {
            write_pair(a,b,i*16+4,0x1000);write_pair(a,b,i*16+6,0x400);
            write_pair(a,b,i*16+14,0x400);write_pair(a,b,i*16+8,0xf);
            write_pair(a,b,i*16+10,0x1fc0);
        }
        write_pair(a,b,0x188,3);
        for(i=0;i<300;++i){spu_advance(a,a->next_sample+63*768);spu_advance(b,b->next_sample+63*768);}
        before=b->control->replay_frames;
        assert(spu_read(a,0x19c)==spu_read(b,0x19c));
        assert(!b->control->background_frames);
        assert(b->control->replay_frames==before && !b->control->voice_reads && !b->control->voice_fallbacks && !b->control->pending_pitch);
        observe(a,b);
        spu_destroy(a);spu_destroy(b);
    }
    puts("PASS register observers need no scratch workspace, including noise inputs");
}
static void unsafe_event_at_capacity(void) {
    spu_t *s=spu_create();assert(s);spu_set_backend(s,&sink,NULL);
    spu_write(s,0x1aa,0xc000);assert(s->control->voice_safe);
    s->control->capacity=s->control->used;
    spu_write(s,0x1a8,1);
    assert(s->control->journal_full==1 && !s->control->voice_safe);
    spu_destroy(s);
    puts("PASS unsafe event remains classified after a capacity-triggered full snapshot");
}
static void off_predecessor_spans(void) {
    unsigned pitch,manual;
    for(manual=0;manual<2;++manual)for(pitch=0;pitch<5;++pitch) {
        static const unsigned pitches[]={0,1,0x1000,0x3fff,0xffff};
        spu_t *a=spu_create(),*b=spu_create();unsigned i;
        assert(a&&b);memset(a->ram+0x2000,0x77,16);a->ram[0x2000]=0;a->ram[0x2001]=7;
        memcpy(b,a,sizeof(*b));a->replay=1;
        spu_set_backend(a,&sink,NULL);spu_set_backend(b,&sink,NULL);
        write_pair(a,b,0x1aa,0xc000);write_pair(a,b,0x190,2);
        for(i=0;i<2;++i){
            write_pair(a,b,i*16+4,pitches[pitch]);write_pair(a,b,i*16+6,0x400);
            write_pair(a,b,i*16+14,0x400);write_pair(a,b,i*16+8,0xf);
            write_pair(a,b,i*16+10,0x1fc0);
        }
        if(manual)write_pair(a,b,0x0c,0x4000); /* Writable ENVX can be nonzero while phase is off. */
        write_pair(a,b,0x188,2); /* Carrier active, predecessor remains off. */
        for(i=0;i<100;++i){spu_advance(a,a->next_sample+63*768);spu_advance(b,b->next_sample+63*768);}
        assert(spu_read(a,0x19c)==spu_read(b,0x19c));
        assert(!b->control->voice_reads);
        assert(a->voices[1].position==b->voices[1].position);
        assert(a->voices[1].fraction==b->voices[1].fraction);observe(a,b);
        spu_destroy(a);spu_destroy(b);
    }
    puts("PASS off PMON predecessor: bulk spans retain zero/signed/clamped carrier pitches");
}
static void empty_manual_mode_view(void) {
    spu_t *a=spu_create(),*b=spu_create();unsigned i;
    assert(a&&b);a->ram[0x2001]=7;memcpy(b,a,sizeof(*b));a->replay=1;
    spu_set_backend(a,&sink,NULL);spu_set_backend(b,&sink,NULL);
    write_pair(a,b,0x1aa,0xc010);write_pair(a,b,0x190,2);
    for(i=0;i<2;++i){
        write_pair(a,b,i*16+4,0x1000);write_pair(a,b,i*16+6,0x400);
        write_pair(a,b,i*16+14,0x400);write_pair(a,b,i*16+8,0xf);
        write_pair(a,b,i*16+10,0x1fc0);
    }
    write_pair(a,b,0x188,3);spu_sync(b);
    assert(b->control->voice_safe && !b->fifo_count);
    write_pair(a,b,0x190,0);write_pair(a,b,0x190,2);
    for(i=0;i<200;++i){spu_advance(a,a->next_sample+63*768);spu_advance(b,b->next_sample+63*768);}
    assert(spu_read(a,0x19c)==spu_read(b,0x19c));assert(!b->control->voice_reads);
    write_pair(a,b,0x1a6,0x8000);write_pair(a,b,0x1a8,0x1234);
    assert(!b->control->voice_safe);
    spu_advance(a,a->next_sample);spu_advance(b,b->next_sample);
    assert(!memcmp(spu_ram(a),spu_ram(b),512*1024));observe(a,b);
    spu_destroy(a);spu_destroy(b);
    puts("PASS empty manual FIFO view and subsequent data-write full fallback");
}
static void journal_pressure(void) {
    spu_t *a=spu_create(), *b=spu_create();
    unsigned i, saw_partial=0, saw_compaction=0, previous_used=0, observed_partial=0;
    uint64_t previous_syncs=0;
    assert(a&&b);
    memset(a->ram+0x2000,0x77,16);a->ram[0x2000]=0;a->ram[0x2001]=7;
    memcpy(b,a,sizeof(*b));a->replay=1;
    spu_set_backend(a,&sink,NULL);spu_set_backend(b,&sink,NULL);
    b->control->capacity=8192;
    write_pair(a,b,0x1aa,0xc000);write_pair(a,b,0x190,2);
    for(i=0;i<2;++i) {
        write_pair(a,b,i*16+4,0x1000);write_pair(a,b,i*16+6,0x400);
        write_pair(a,b,i*16+14,0x400);write_pair(a,b,i*16+8,0xf);
        write_pair(a,b,i*16+10,0x1fc0);
    }
    write_pair(a,b,0x188,3);write_pair(a,b,0x1a6,0x8000);
    for(i=0;i<2400;++i) {
        write_pair(a,b,0,i*313u);
        /* Adjacent DMA words coalesce, including after prefix compaction. */
        spu_dma_write(a,i);spu_dma_write(b,i);
        spu_dma_write(a,~i);spu_dma_write(b,~i);
        spu_advance(a,a->next_sample+63*768);spu_advance(b,b->next_sample+63*768);
        if(b->control->background_active&&b->control->head) {
            saw_partial=1;
            assert(b->control->shadow->next_sample<=b->next_sample);
        }
        if(b->control->used<previous_used && b->control->syncs==previous_syncs)saw_compaction=1;
        previous_used=b->control->used;
        previous_syncs=b->control->syncs;
        if(!observed_partial && i>200 && b->control->background_active && b->control->head) {
            observed_partial=1;
            /* A first ENDX read must resume a partially consumed history. */
            assert(b->control->background_active&&b->control->head);
            assert(spu_read(a,0x19c)==spu_read(b,0x19c));observe(a,b);
        }
    }
    assert(saw_partial&&saw_compaction&&observed_partial&&b->control->background_frames);
    assert(!b->control->journal_full);
    assert(b->control->background_max_samples<=128);
    assert(b->control->background_max_bytes<=5136);observe(a,b);
    {
        int16_t cd[8192],left[512],right[512];unsigned got;
        while((got=spu_read_frames(a,left,256))) {
            assert(got==spu_read_frames(b,right,256));assert(!memcmp(left,right,got*4));
        }
        assert(!spu_read_frames(b,right,256));
        for(i=0;i<8192;++i)cd[i]=(int16_t)(i*71u);
        spu_cd_input(a,cd,4096);spu_cd_input(b,cd,4096);
        /* A producer burst may still materialize; live CD queues also agree. */
        assert(a->cd_count==b->cd_count&&a->cd_read==b->cd_read);
        assert(!memcmp(a->cd,b->cd,sizeof(a->cd)));observe(a,b);
        spu_advance(a,a->next_sample+1023*768);spu_advance(b,b->next_sample+1023*768);
        while((got=spu_read_frames(a,left,256))) {
            assert(got==spu_read_frames(b,right,256));assert(!memcmp(left,right,got*4));
        }
        assert(!spu_read_frames(b,right,256));observe(a,b);
    }
    for(i=0;i<2400;++i) {
        write_pair(a,b,0,i*71u);
        spu_advance(a,a->next_sample+63*768);spu_advance(b,b->next_sample+63*768);
        if(b->control->background_active&&b->control->head)break;
    }
    assert(i<2400);
    /* The API forbids backend changes after synthesis; it must retain history. */
    spu_set_backend(b,NULL,NULL);assert(b->control&&b->control->background_active);
    spu_sync(b);assert(!memcmp(a,b,offsetof(spu_t,output)));
    spu_destroy(a);spu_destroy(b);
    puts("PASS bounded journal pressure: partial observers, DMA coalescing, compaction and backend-change rejection");
}
int main(void) {
    spu_t *a=spu_create(),*b=spu_create();uint32_t seed=24601;unsigned n,i,deferred=0;
    irq_log_t left={a,0,{0}},right={b,0,{0}};
    first_observers();sparse_history_observer();repeated_voice_observers();voice_then_ram_observer();voice_workspace_fallback();unsafe_event_at_capacity();off_predecessor_spans();empty_manual_mode_view();journal_pressure();
    assert(a&&b);a->replay=1;spu_set_backend(a,&sink,NULL);spu_set_backend(b,&sink,NULL);
    spu_set_irq(a,irq,&left);spu_set_irq(b,irq,&right);
    write_pair(a,b,0x1aa,0xc080);write_pair(a,b,0x1a2,0xe000);write_pair(a,b,0x1ac,4);
    for(n=0;n<24;++n){
        unsigned address=0x2000+n*128;
        for(i=0;i<128;i+=4){write_pair(a,b,0x1a6,(address+i)/8);spu_dma_write(a,0x77770700);spu_dma_write(b,0x77770700);}
        write_pair(a,b,n*16+4,0x800+n*193);write_pair(a,b,n*16+6,address/8);
        write_pair(a,b,n*16+14,address/8);write_pair(a,b,n*16+8,0x0f);write_pair(a,b,n*16+10,0x1fc0);
    }
    write_pair(a,b,0x188,0xffff);write_pair(a,b,0x18a,0xff);
    for(i=0;i<240;++i){
        unsigned mode=i%12,count=1+random_word(&seed)%4096;
        int16_t cd[512],x[512],y[512];unsigned got;
        if(mode==0){write_pair(a,b,0x190,0xaaaa);write_pair(a,b,0x192,0xaa);}
        if(mode==1){ /* IRQ enabling before a sample must also restore deferred histories. */
            write_pair(a,b,0x1aa,0xc0c0);
        }
        if(mode==2)write_pair(a,b,0x1aa,0xc080);
        if(mode==3){write_pair(a,b,0x190,0);write_pair(a,b,0x192,0);}
        if(mode==4){write_pair(a,b,0x190,2);write_pair(a,b,0x1c0,0x1234);}
        if(mode==5){write_pair(a,b,0x18c,0xaaaa);write_pair(a,b,0x188,0x5555);}
        if(mode==6){unsigned voice=random_word(&seed)%24;write_pair(a,b,voice*16+4,random_word(&seed));}
        if(mode==7){write_pair(a,b,0x1aa,0xc0c0);write_pair(a,b,0x1a4,0x400);}
        if(mode==8){write_pair(a,b,0x1aa,0xc080);write_pair(a,b,0x190,0xfffe);}
        if(mode==9){write_pair(a,b,0x1a6,0xe000);assert(spu_dma_read(a)==spu_dma_read(b));}
        if(mode==10){ /* One-shot data and an old PMON target remain observable after PMON clears. */
            write_pair(a,b,0x1a6,0x400);spu_dma_write(a,0x77770100);spu_dma_write(b,0x77770100);
        }
        for(n=0;n<512;++n)cd[n]=(int16_t)random_word(&seed);
        spu_cd_input(a,cd,256);spu_cd_input(b,cd,256);
        spu_advance(a,a->next_sample+(count-1)*768);spu_advance(b,b->next_sample+(count-1)*768);
        deferred+=!!b->control->pending_pitch;
        assert(left.count==right.count&&!memcmp(left.at,right.at,left.count*sizeof(uint64_t)));
        left.count=right.count=0;
        got=spu_read_frames(a,x,256);assert(got==spu_read_frames(b,y,256));assert(!memcmp(x,y,got*4));
        /* Independent envelopes do not force the pitch journal to replay. */
        assert(spu_read(a,12)==spu_read(b,12));
        if(i%3==2)observe(a,b);
    }
    observe(a,b);assert(!deferred);
    spu_destroy(a);spu_destroy(b);printf("PASS current native pitch: %u deferred spans; registers, RAM, raw CD and IRQs agree\n",deferred);return 0;
}
