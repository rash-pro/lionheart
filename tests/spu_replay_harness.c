#include "../runtime/spu.c"
#include <assert.h>
#include <stdio.h>

static uint32_t random_word(uint32_t *state) { *state = *state * 1664525u + 1013904223u; return *state; }
#ifdef FF8_PLAY
static void silent_replay_dependencies(void) {
    static const spu_backend_t sink={NULL,NULL};
    unsigned manual,key,n,i;
    for(manual=0;manual<2;++manual)for(key=0;key<2;++key) {
        spu_t *a=spu_create(),*b=malloc(sizeof(*b));spu_control_t replay={0};
        assert(a&&b);a->backend=&sink;a->replay=1;
        REG(a,0x1aa)=0xc080;REG(a,0x1ac)=4;REG(a,0x1a2)=0xf000;
        REG(a,0x190)=0xfffe;REG(a,0x192)=0xff;REG(a,0x198)=0xffff;REG(a,0x19a)=0xff;
        memset(a->ram+0x2000,0x77,16);a->ram[0x2000]=0;a->ram[0x2001]=7;
        for(n=0;n<24;++n) {
            REG(a,n*16+4)=0x1000;REG(a,n*16+6)=REG(a,n*16+14)=0x400;
            a->voices[n].address=0x2000;
            REG(a,n*16+8)=0xf;REG(a,n*16+10)=0x1fc0;
        }
        /* OFF alone does not imply silence; manual ENVX and pending KON must
         * keep the affected output and all downstream PMON dependencies. */
        if(manual)a->voices[0].adsr.level=0x4000;
        if(key)a->key_on=1;
        memcpy(b,a,sizeof(*b));replay.shadow=b;
        for(i=0;i<3;++i) {
            uint64_t until=a->next_sample+255*SPU_SAMPLE_CYCLES;
            while(a->next_sample<=until){sample(a);a->next_sample+=SPU_SAMPLE_CYCLES;}
            control_replay(&replay,until);
            assert(!memcmp(a,b,offsetof(spu_t,output)));
            assert(!memcmp(a->output,b->output,sizeof(a->output)));
            assert(a->frames==b->frames&&a->next_sample==b->next_sample&&a->blocks==b->blocks);
        }
        if(!manual&&!key)assert(replay.replay_voice_frames==24u*768);
        spu_destroy(a);spu_destroy(b);
    }
    puts("PASS silent replay: complete RAM/PCM/state match with OFF ENVX and pending KON dependencies");
}
#endif
int main(void) {
#ifdef FF8_PLAY
    silent_replay_dependencies();
#endif
    static const spu_backend_t sink = {NULL, NULL};
    uint32_t seed = 2201;
    unsigned capture, mode, n, iteration;
    uint64_t batched = 0, aliases = 0, fifos = 0;
    for (capture = 0; capture < 2; ++capture) for (mode = 0; mode < 8; ++mode) {
        spu_t *a = spu_create(), *b = malloc(sizeof(*b));
        spu_control_t replay = {0};
        assert(a && b);
        a->backend = &sink; a->replay = 1;
        REG(a,0x1aa) = mode & 1 ? 0xc0c0 : 0xc040;
        REG(a,0x1ac) = capture ? 4 : 0;
        REG(a,0x1a2) = 0xf000; REG(a,0x1a4) = 0x41;
        a->reverb_address = 0x78000;
        REG(a,0x190) = mode & 2 ? 0xfffe : 0;
        REG(a,0x192) = mode & 2 ? 0x81 : 0;
        REG(a,0x194) = mode & 4 ? 0xaaaa : 0;
        REG(a,0x198) = mode & 4 ? 0x1111 : 0;
        for (n = 0; n < sizeof(a->ram); ++n) a->ram[n] = (uint8_t)(random_word(&seed) >> 24);
        for (n = 0; n < 24; ++n) {
            unsigned base = n % 3 == 0 ? 0x20000 + n * 256 : n % 3 == 1 ? 0 : 0x78000;
            REG(a,n*16+4) = (uint16_t)random_word(&seed);
            REG(a,n*16+6) = REG(a,n*16+14) = (uint16_t)(base / 8);
            REG(a,n*16+8) = (uint16_t)random_word(&seed);
            REG(a,n*16+10) = (uint16_t)random_word(&seed);
            REG(a,n*16) = (uint16_t)random_word(&seed);
            a->volume_work[0] |= 1u << n;
            a->volume_sweep[0] |= REG(a,n*16) & 0x8000 ? 1u << n : 0;
            a->ram[base+1] = 7;
        }
        a->key_on = 0xffffff;
        memcpy(b,a,sizeof(*b)); replay.shadow = b;
        for (iteration = 0; iteration < 12; ++iteration) {
            unsigned count = 1 + random_word(&seed) % 4096;
            uint64_t until = a->next_sample + (count - 1) * SPU_SAMPLE_CYCLES;
            /* Bypass the old legacy bulk path to keep the oracle independent. */
            while (a->next_sample <= until) { sample(a); a->next_sample += SPU_SAMPLE_CYCLES; }
            control_replay(&replay,until);
            assert(!memcmp(a,b,offsetof(spu_t,output)));
            assert(a->next_sample == b->next_sample && a->frames == b->frames && a->blocks == b->blocks);
            assert(!memcmp(a->output,b->output,sizeof(a->output)));
            assert(a->output_read == b->output_read && a->output_count == b->output_count);
            if (iteration == 4) { a->fifo[a->fifo_count++] = b->fifo[b->fifo_count++] = 0x1234; }
            if (iteration == 7) { REG(a,0x190) ^= 0x2222; REG(b,0x190) = REG(a,0x190); }
        }
        batched += replay.replay_voice_frames; aliases += replay.replay_alias_spans; fifos += replay.replay_fifo_spans;
        spu_destroy(a); spu_destroy(b);
    }
    assert(batched && aliases && fifos);
    printf("PASS replay spans voice-samples=%llu alias-spans=%llu fifo-spans=%llu\n",
        (unsigned long long)batched,(unsigned long long)aliases,(unsigned long long)fifos);
    return 0;
}
