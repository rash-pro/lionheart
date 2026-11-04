/* Synthetic noise clock transitions; no game data or emulator oracle. */
#include "../runtime/spu.c"
#include <assert.h>

static const spu_backend_t sink = {NULL, NULL};

static void advance(spu_t *s, unsigned count) {
    spu_advance(s, s->next_sample + (uint64_t)(count - 1) * SPU_SAMPLE_CYCLES);
}

static void transitions(void) {
    spu_t *s = spu_create();
    spu_write(s, 0x1aa, 0xc000);
    advance(s, 1);
    assert(s->noise == 1);
    /* A slow clock has just reloaded. Selecting shift 12 must take effect
     * within its new eight-sample period, including before a voice's KON. */
    spu_write(s, 0x1aa, 0xf000);
    advance(s, 8);
    assert(s->noise == 3);
    /* KON is per voice; it must not reset this shared oscillator. A write
     * after KON, including its pending latch and two-sample delay, uses the
     * same clock as a write before KON. */
    spu_write(s, 0x188, 1);
    spu_write(s, 0x1aa, 0xfc00);
    advance(s, 1);
    assert(s->noise == 7 && s->voices[0].delay == 1);
    spu_write(s, 0x188, 1);
    advance(s, 1);
    assert(s->noise == 15);
    spu_write(s, 0x18c, 1);
    advance(s, 1);
    assert(s->noise == 31);
    /* Slowing the clock also takes effect immediately, preserving phase. */
    spu_write(s, 0x1aa, 0xc000);
    advance(s, 100);
    assert(s->noise == 31);
    spu_destroy(s);
}

static void paths(unsigned fallback) {
    spu_t *a = spu_create(), *b = spu_create();
    static const unsigned rates[] = {0xc000, 0xf000, 0xff00, 0xc100, 0xdc00, 0xfc00, 0xc000};
    unsigned i, n;
    /* A noise modulator and an audible synthetic carrier. Capture keeps the
     * replay's PCM dependency live; volume zero does not remove PMON input. */
    for (i = 0; i < 2; ++i) {
        spu_t *s = i ? b : a;
        for (n = 0; n < 2; ++n) {
            memset(s->ram + 0x2000 + n*16, 0x31, 16);
            s->ram[0x2000+n*16] = 0; s->ram[0x2001+n*16] = 3;
            spu_write(s,n*16+4,0x1000); spu_write(s,n*16+6,0x400+n*2);
            spu_write(s,n*16+14,0x400+n*2);
            spu_write(s,n*16+8,0x000f); spu_write(s,n*16+10,0x1fc0);
        }
        spu_write(s,0x194,1); spu_write(s,0x190,2);
        spu_write(s,0x1ac,4); spu_write(s,0x1a2,0xf000);
    }
    spu_set_backend(b,&sink,NULL);
    if (fallback) { control_destroy(b); b->control = NULL; }
    for (i = 0; i < sizeof(rates)/sizeof(rates[0]); ++i) {
        spu_write(a,0x1aa,rates[i]); spu_write(b,0x1aa,rates[i]);
        if (i == 1) { spu_write(a,0x188,3); spu_write(b,0x188,3); }
        advance(a,4096); advance(b,4096);
        spu_sync(b);
        assert(a->noise == b->noise && a->noise_timer == b->noise_timer);
        assert(!memcmp(a->voices,b->voices,sizeof(a->voices)));
        assert(!memcmp(a->ram,b->ram,sizeof(a->ram)));
    }
    spu_destroy(a); spu_destroy(b);
}

static void constant_rates(void) {
    unsigned rate;
    for (rate = 0; rate < 64; ++rate) {
        spu_t *s = spu_create();
        unsigned period = 0x20000u >> (rate >> 2), step = 4 + (rate & 3);
        unsigned edges = (4096u*step + period-1)/period, i;
        uint16_t expected = 0;
        if (edges > 4096) edges = 4096;
        for (i = 0; i < edges; ++i) {
            unsigned parity = 1u ^ (expected >> 15) ^ (expected >> 12) ^ (expected >> 11) ^ (expected >> 10);
            expected = (uint16_t)(expected*2 + (parity & 1));
        }
        spu_write(s,0x1aa,0xc000u | (rate << 8));
        advance(s,4096);
        assert(s->noise == expected);
        spu_destroy(s);
    }
}

#if defined(FF8_SAVESTATES) && !defined(FF8_PLAY)
static void checkpoint(void) {
    spu_t *a = spu_create(), *b = spu_create();
    ss_io out = {0}, in;
    int16_t pcm[128], restored[128];
    spu_write(a,0x1aa,0xf100); advance(a,3);
    assert(a->noise_timer && a->noise_timer < 0x20000);
    ss_spu(&out,a); assert(!out.failed && out.version == 3);
    in = out; in.pos = 0; in.reading = 1;
    ss_spu(&in,b); assert(!in.failed && in.pos == out.size);
    assert(a->noise_timer == b->noise_timer && a->noise == b->noise);
    advance(a,32); advance(b,32);
    assert(a->noise_timer == b->noise_timer && a->noise == b->noise);
    assert(spu_read_frames(a,pcm,64) == spu_read_frames(b,restored,64));
    assert(!memcmp(pcm,restored,35*4));
    free(out.data); spu_destroy(a); spu_destroy(b);
}
#endif

int main(void) {
    transitions(); constant_rates(); paths(0); paths(1);
#if defined(FF8_SAVESTATES) && !defined(FF8_PLAY)
    checkpoint();
#endif
    puts("PASS noise rate transitions, KON/KOFF, PMON, canonical replay and backend fallback");
    return 0;
}
