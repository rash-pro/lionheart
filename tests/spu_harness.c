/* Numeric reference vectors and public MMIO/DMA integration, all synthetic. */
#include "hw.h"
#include "../runtime/spu.c"
#include <assert.h>

static void tick(spu_t *s, unsigned n) { spu_advance(s, s->next_sample + (uint64_t)(n - 1) * 768); }
static void upload(spu_t *s, unsigned address, const uint8_t *data, unsigned size) {
    unsigned i;
    spu_write(s, 0x1a6, address / 8);
    for (i = 0; i < size; i += 4)
        spu_dma_write(s, data[i] | ((uint32_t)data[i+1] << 8) | ((uint32_t)data[i+2] << 16) | ((uint32_t)data[i+3] << 24));
}
static void tone(spu_t *s, unsigned n, unsigned loop) {
    uint8_t block[16];
    memset(block, 0x11, sizeof(block)); block[0] = 0; block[1] = (uint8_t)loop;
    upload(s, 0x1000, block, sizeof(block));
    spu_write(s, n * 16, 0x3fff); spu_write(s, n * 16 + 2, 0x3fff);
    spu_write(s, n * 16 + 4, 0x1000); spu_write(s, n * 16 + 6, 0x200);
    spu_write(s, n * 16 + 8, 0x00ff); spu_write(s, n * 16 + 10, 0x1fc0);
    spu_write(s, 0x180, 0x3fff); spu_write(s, 0x182, 0x3fff);
    spu_write(s, 0x1ac, 4); spu_write(s, 0x1aa, 0xc000);
    spu_write(s, 0x188 + (n / 16) * 2, 1u << (n & 15));
}

static void adpcm(void) {
    uint8_t block[16] = {0};
    int16_t history[2] = {0,0}, out[28];
    const int16_t golden[] = {4096,-4096,28672,-32768,0,8192,-8192,12288};
    const int16_t predictive[] = {4096,3840,3600,3375,3164,2966,2781,2607};
    unsigned i;
    block[2] = 0xf1; block[3] = 0x87; block[4] = 0x20; block[5] = 0x3e;
    decode_block(block, history, out);
    for (i = 0; i < 8; ++i) assert(out[i] == golden[i]);
    memset(block, 0, sizeof(block)); memset(history, 0, sizeof(history));
    block[0] = 0x10; block[2] = 1; decode_block(block, history, out);
    for (i = 0; i < 8; ++i) assert(out[i] == predictive[i]);
    block[0] = 0x4c; memset(block + 2, 0, 14); history[0] = 1000; history[1] = -1000;
    decode_block(block, history, out); assert(out[0] == 2844 && out[1] == 4484);
    block[0] = 0x10; memset(block + 2, 0x77, 14); history[0] = history[1] = 32767;
    decode_block(block, history, out); for (i = 0; i < 28; ++i) assert(out[i] == 32767);
    block[0] = 15; memset(block + 2, 0x81, 14); history[0] = history[1] = 0;
    decode_block(block, history, out); assert(out[0] == 8 && out[1] == -64);
}

static void gauss(void) {
    unsigned i;
    int16_t constant[4] = {32767,32767,32767,32767}, impulse[4] = {0,32767,0,0};
    assert(gaussian[0] == -1 && gaussian[16] == 0 && gaussian[128] == 424);
    assert(gaussian[255] == 4807 && gaussian[256] == 4871 && gaussian[511] == 22963);
    for (i = 0; i < 256; ++i) {
        int sum = gaussian[i] + gaussian[255-i] + gaussian[256+i] + gaussian[511-i];
        assert(sum >= 0x7f7f && sum <= 0x7f81);
        assert(interpolate(constant, i * 16) >= 32635 && interpolate(constant, i * 16) <= 32640);
    }
    assert(interpolate(impulse, 0) == 22962);
    assert(interpolate(impulse, 0xff0) == 4870);
}

static void adsr(void) {
    voice_t v = {0};
    envelope_t e = {0,0};
    unsigned i;
    v.phase = ENV_ATTACK;
    adsr_tick(&v, 0x0000, 0x4000); assert(v.adsr.level == 14336 && v.phase == ENV_ATTACK);
    adsr_tick(&v, 0x0000, 0x4000); assert(v.adsr.level == 28672 && v.phase == ENV_ATTACK);
    adsr_tick(&v, 0x0000, 0x4000); assert(v.adsr.level == 32767 && v.phase == ENV_DECAY);
    for (i = 0; i < 4; ++i) adsr_tick(&v, 0, 0x4000);
    assert(v.adsr.level == 2047 && v.phase == ENV_SUSTAIN);
    adsr_tick(&v, 0, 0x4000); assert(v.adsr.level == 0 && v.phase == ENV_SUSTAIN);
    v.phase = ENV_RELEASE; v.adsr.level = 32767;
    adsr_tick(&v, 0, 0); assert(v.adsr.level == 16383);
    adsr_tick(&v, 0, 0); assert(v.adsr.level == 0 && v.phase == ENV_OFF);
    /* Exponential attack uses a smaller STEP at fast rates, not four idle clocks. */
    v.phase = ENV_ATTACK; v.adsr.level = 28672;
    adsr_tick(&v, 0x8000, 0); assert(v.adsr.level == 32256 && v.phase == ENV_ATTACK);
    adsr_tick(&v, 0x8000, 0); assert(v.adsr.level == 32767 && v.phase == ENV_DECAY);
    for (i = 0; i < 65535; ++i) envelope_tick(&e, 27, 2, 0, 0, 0, 0);
    assert(e.level == 5); /* Minimum increment is one: a step every 32768 clocks. */
    e.level = 1234; e.counter = 0;
    for (i = 0; i < 100000; ++i) envelope_tick(&e, 31, 3, 0, 0, 0, 1);
    assert(e.level == 1234);
    v.phase = ENV_RELEASE; v.adsr.level = 20000; v.adsr.counter = 0;
    for (i = 0; i < 100000; ++i) adsr_tick(&v, 0, 31);
    assert(v.adsr.level == 20000);
    e.level = 0; e.counter = 0; volume_tick(&e, 0x9000); assert(e.level == -16384);
    volume_tick(&e, 0x9000); assert(e.level == -32768);
    volume_tick(&e, 0xb000); assert(e.level == -18432);
    e.level = 1000; volume_tick(&e, 0x907f); assert(e.level == 1000);
}

static void known_sample(void) {
    static const int16_t expected[] = {0,0,0,263,3040,4074,4074,4074,4074,4074,4074,4074};
    spu_t *s = spu_create(); int16_t pcm[24]; unsigned i;
    tone(s, 0, 7);
    spu_advance(s, 767); assert(spu_read_frames(s, pcm, 12) == 0);
    spu_advance(s, 768 * 12); assert(spu_read_frames(s, pcm, 12) == 12);
    for (i = 0; i < 12; ++i) {
        if (pcm[i*2] != expected[i]) fprintf(stderr, "sample %u: %d expected %d\n", i, pcm[i*2], expected[i]);
        assert(pcm[i*2] == expected[i] && pcm[i*2+1] == expected[i]);
    }
    assert(spu_next_event(s) == 768 * 13);
    spu_destroy(s);
}

static void loops(void) {
    spu_t *s = spu_create();
    tone(s, 23, 7); tick(s, 29); assert(!(spu_read(s, 0x19e) & 128));
    tick(s, 1); assert(spu_read(s, 0x19e) & 128); assert(spu_read(s, 23*16+14) == 0x200);
    spu_write(s, 0x18a, 128); tick(s, 1); assert(!(spu_read(s, 0x19e) & 128));
    tick(s, 10); spu_write(s, 0x18e, 128); tick(s, 2); assert(spu_read(s, 23*16+12) == 0);
    tone(s, 0, 1); tick(s, 30); assert(spu_read(s, 0x19c) & 1); assert(!spu_read(s, 12));
    spu_destroy(s);
}

static unsigned irqs;
static void irq(void *unused) { (void)unused; ++irqs; }
static void transfers(void) {
    spu_t *s = spu_create();
    spu_set_irq(s, irq, NULL);
    spu_write(s, 0x1a4, 0x200); spu_write(s, 0x1aa, 0xc040);
    spu_write(s, 0x1a6, 0x200); spu_write(s, 0x1a8, 0x1234); spu_write(s, 0x1a8, 0xabcd);
    tick(s, 1); assert(!irqs && s->ram[0x1000] == 0);
    spu_write(s, 0x1aa, 0xc050); assert(spu_read(s, 0x1ae) & 0x400);
    tick(s, 1); assert(irqs == 1 && (spu_read(s, 0x1ae) & 0x7f) == 0x50);
    assert(!(spu_read(s, 0x1ae) & 0x400));
    spu_write(s, 0x1a6, 0x200); assert(spu_dma_read(s) == 0xabcd1234u);
    assert(spu_read(s, 0x1a6) == 0x200 && irqs == 1);
    spu_write(s, 0x1aa, 0xc030); assert(!(spu_read(s, 0x1ae) & 0x40));
    tick(s, 1); assert((spu_read(s, 0x1ae) & 0x3ff) == 0x2b0);
    spu_write(s, 0x1a6, 0xffff); spu_dma_write(s, 1); spu_dma_write(s, 2); spu_dma_write(s, 3);
    spu_write(s, 0x1a6, 0xffff); assert(spu_dma_read(s) == 1 && spu_dma_read(s) == 2 && spu_dma_read(s) == 3);
    /* Capture and inaudible ADPCM reads also assert the latched IRQ. */
    spu_write(s, 0x1a4, 0); spu_write(s, 0x1ac, 4); spu_write(s, 0x1aa, 0xc040);
    tick(s, 1); assert(irqs == 2);
    spu_destroy(s);
}

static void dma(void) {
    cpu_t *c = malloc(sizeof(*c)); unsigned i; int16_t pcm[4];
    cpu_init(c); hw_get(c);
    for (i = 0; i < 32; ++i) mem_write32(c, 0x10000 + i * 4, 0xab000000u + i * 0x10101u);
    hw_write(c, 0x1f801da4, 2, 0x200); hw_write(c, 0x1f801da6, 2, 0x200);
    hw_write(c, 0x1f801daa, 2, 0xc060);
    hw_write(c, 0x1f8010f0, 4, 8u << 16);
    hw_write(c, 0x1f8010f4, 4, (1u << 23) | (1u << 20));
    hw_write(c, 0x1f8010c0, 4, 0x10000); hw_write(c, 0x1f8010c4, 4, 0x20010);
    hw_write(c, 0x1f8010c8, 4, 0x01000201);
    c->cycles += 1040; hw_event(c);
    assert(c->hw->dma[4].completions == 1 && !(hw_read(c, 0x1f8010c8, 4) & 0x1000000));
    assert((c->hw->i_stat & 0x208) == 0x208 && (c->hw->dicr & 0x90000000u) == 0x90000000u);
    hw_write(c, 0x1f801da6, 2, 0x200); hw_write(c, 0x1f801daa, 2, 0xc030);
    hw_write(c, 0x1f8010c0, 4, 0x20000); hw_write(c, 0x1f8010c4, 4, 0x20010);
    hw_write(c, 0x1f8010c8, 4, 0x01000200); c->cycles += 1040; hw_event(c);
    for (i = 0; i < 32; ++i) assert(mem_read32(c, 0x20000 + i * 4) == 0xab000000u + i * 0x10101u);
    hw_write(c, 0x1f801d80, 4, 0x12345678); assert(hw_read(c, 0x1f801d80, 4) == 0x12345678);
    hw_write(c, 0x1f801d81, 1, 0); assert(hw_read(c, 0x1f801d80, 2) == 0x5678);
    while (spu_read_frames(c->hw->spu, pcm, 2)) {}
    {
        uint64_t first = spu_next_event(c->hw->spu), due = spu_batch_deadline(c->hw->spu);
        unsigned frames = 0, n;
        c->cycles = due - 1; hw_event(c); assert(spu_read_frames(c->hw->spu, pcm, 2) == 0);
        ++c->cycles; hw_event(c);
        while ((n = spu_read_frames(c->hw->spu, pcm, 2))) frames += n;
        assert(frames == (due - first) / SPU_SAMPLE_CYCLES + 1);
    }
    hw_destroy(c); free(c);
}

static void stream_dma(void) {
    cpu_t *c = malloc(sizeof(*c)); unsigned i;
    cpu_init(c); hw_get(c);
    for (i = 0; i < 128; ++i) mem_write32(c, 0x10000 + 4 * i, 0x12340000u + i);
    hw_write(c, 0x1f801da4, 2, 0x201); hw_write(c, 0x1f801da6, 2, 0x200);
    hw_write(c, 0x1f801daa, 2, 0xc020);
    hw_write(c, 0x1f8010f0, 4, 8u << 16);
    hw_write(c, 0x1f8010c0, 4, 0x10000); hw_write(c, 0x1f8010c4, 4, 0x10080);
    hw_write(c, 0x1f8010c8, 4, 0x01000201);
    c->cycles = 128; hw_event(c);
    assert(hw_read(c, 0x1f8010c8, 4) & 0x1000000);
    hw_write(c, 0x1f801daa, 2, 0xc060);
    c->cycles = 4111; hw_event(c);
    assert(!(c->hw->i_stat & 0x200));
    assert(!c->hw->dma[4].completions);
    ++c->cycles; hw_event(c); assert(c->hw->dma[4].completions == 1);
    assert(!(c->hw->i_stat & 0x200) && !(spu_read(c->hw->spu, 0x1ae) & 0x40));
    hw_write(c, 0x1f801da6, 2, 0x200); hw_write(c, 0x1f801daa, 2, 0xc030);
    for (i = 0; i < 128; ++i) assert(spu_dma_read(c->hw->spu) == 0x12340000u + i);
    hw_write(c, 0x1f801daa, 2, 0xc040);
    hw_write(c, 0x1f801c04, 2, 0x1000); hw_write(c, 0x1f801c06, 2, 0x200);
    hw_write(c, 0x1f801d88, 2, 1); c->cycles += 4 * 768; hw_event(c);
    assert(c->hw->i_stat & 0x200);
    hw_destroy(c); free(c);
}

static void cd_mix(void) {
    spu_t *s = spu_create();
    int16_t input[] = {10000,-10000,20000,-20000}, out[8];
    spu_write(s, 0x180, 0x3fff); spu_write(s, 0x182, 0x3fff);
    spu_write(s, 0x1b0, 0x4000); spu_write(s, 0x1b2, 0x4000);
    spu_write(s, 0x1ac, 4); spu_write(s, 0x1aa, 1); /* CD works with voices disabled/muted. */
    spu_cd_input(s, input, 2); tick(s, 3); assert(spu_read_frames(s, out, 4) == 3);
    assert(out[0] == 4999 && out[1] == -5000 && out[2] == 9999 && out[3] == -10000);
    assert(out[4] == 0 && out[5] == 0);
    assert((int16_t)spu_ram_read(s, 2) == 20000 && (int16_t)spu_ram_read(s, 0x402) == -20000);
    spu_write(s, 0x1aa, 0); spu_cd_input(s, input, 2); tick(s, 2); spu_read_frames(s, out, 4);
    assert(out[0] == 0 && out[1] == 0 && out[2] == 0 && out[3] == 0);
    tick(s, AUDIO_FRAMES + 1); assert(s->dropped == 1);
    spu_destroy(s);
}

static void modulation(void) {
    spu_t *s = spu_create();
    voice_t *v;
    tone(s, 1, 7); tick(s, 3); v = &s->voices[1];
    v->fraction = v->position = 0; voice_sample(s, 1, -32768, 2, 0); assert(v->position == 0);
    voice_sample(s, 1, 0, 2, 0); assert(v->position == 1);
    voice_sample(s, 1, 32767, 2, 0); assert(v->position == 2 && v->fraction == 4095);
    spu_write(s, 20, 0xffff); voice_sample(s, 1, 0, 0, 0); assert(v->position == 6);
    s->noise_timer = 0; s->noise = 0; spu_write(s, 0x1aa, 0xfc00);
    tick(s, 1); assert(s->noise == 1);
    tick(s, 1); assert(s->noise == 3);
    tick(s, 1); assert(s->noise == 7);
    spu_destroy(s);
}

static void reverb_addresses(void) {
    spu_t *s = spu_create();
    uint32_t seed = 8;
    unsigned base, i;
    int16_t input[2] = {12345, -23456}, output[2];
    reverb_core(s, input, output);
    assert(s->reverb_size == 0x80000);
    for (base = 0; base < 0x10000; ++base) {
        uint32_t size = 0x80000 - base * 8;
        spu_write(s, 0x1a2, (uint16_t)base);
        for (i = 0; i < 32; ++i) {
            seed = seed * 1664525u + 1013904223u;
            spu_write(s, 0x1c0 + i * 2, (uint16_t)(seed >> 16));
        }
        s->reverb_address = base * 8 + (seed % size & ~1u);
        for (i = 0; i < 2; ++i) {
            unsigned tap;
            spu_write(s, 0x1aa, i ? 0xc080 : 0xc000);
            /* Change a tap after the first cache fill, including tiny rings. */
            spu_write(s, 0x1f4, (uint16_t)(base + i));
            reverb_core(s, input, output);
            for (tap = 0; tap < 32; ++tap)
                assert(reverb_cached_address(s, s->reverb_offset[tap]) ==
                       reverb_address(s, (int32_t)REG(s, 0x1c0 + tap * 2) * 8));
            for (tap = 0; tap < 4; ++tap) {
                unsigned dest = (tap / 2 ? 18 : 10) + (tap & 1);
                assert(reverb_cached_address(s, s->reverb_previous[tap]) ==
                       reverb_address(s, (int32_t)REG(s, 0x1c0 + dest * 2) * 8 - 2));
                assert(reverb_cached_address(s, s->reverb_delayed[tap]) == reverb_address(s,
                    ((int32_t)REG(s, 0x1c0 + (26 + tap) * 2) - REG(s, 0x1c0 + (tap / 2) * 2)) * 8));
            }
        }
    }
    spu_destroy(s);
}

static void reverb(void) {
    spu_t *s = spu_create();
    int16_t in[2] = {10000,-10000}, out[2];
    unsigned i; int found = 0;
    int32_t wet[2], input[2] = {0,0};
    spu_write(s, 0x1a2, 0xff00); spu_write(s, 0x1ac, 4);
    /* Disabled reverb must still read the all-pass delay RAM. */
    spu_write(s, 0x1f8, 2); spu_write(s, 0x1fa, 3);
    spu_ram_write(s, 0x7f810, 12345); spu_ram_write(s, 0x7f818, (uint16_t)-12345);
    reverb_core(s, in, out); assert(out[0] == 12345 && out[1] == -12345);
    assert(spu_ram_read(s, 0x7f810) == 12345);
    assert(reverb_address(s, -4) == 0x7fffe && reverb_address(s, 0x800) == 0x7f802);
    /* IIR -> COMB -> APF path: distinct offsets prevent accidental aliasing. */
    memset(s->ram + 0x7f800, 0, 0x800); spu_write(s, 0x1a2, 0xff00);
    spu_write(s, 0x1aa, 0xc080); spu_write(s, 0x1c4, 0x7fff); spu_write(s, 0x1c6, 0x7fff);
    spu_write(s, 0x1fc, 0x7fff); spu_write(s, 0x1fe, 0x7fff);
    spu_write(s, 0x1d4, 8); spu_write(s, 0x1d6, 16); spu_write(s, 0x1d8, 8); spu_write(s, 0x1da, 16);
    spu_write(s, 0x1f4, 24); spu_write(s, 0x1f6, 32); spu_write(s, 0x1f8, 40); spu_write(s, 0x1fa, 48);
    reverb_core(s, in, out);
    assert((int16_t)spu_ram_read(s, 0x7f840) == 9998 && (int16_t)spu_ram_read(s, 0x7f880) == -10000);
    assert((int16_t)spu_ram_read(s, 0x7f8c0) == 9997);
    /* FIR output emits delayed RAM audio even when reverb writes are off. */
    spu_write(s, 0x1aa, 0xc000); spu_write(s, 0x184, 0x7fff);
    for (i = 0; i < 0x800; i += 2) spu_ram_write(s, 0x7f800 + i, 10000);
    for (i = 0; i < 100; ++i) { reverb_sample(s, input, wet); if (wet[0] > 9000) found = 1; }
    assert(found);
    spu_destroy(s);
}

typedef struct { spu_t *s; uint64_t cycle; } irq_clock_t;
static void irq_clock(void *context) {
    irq_clock_t *clock = context;
    clock->cycle = spu_next_event(clock->s);
}
static void deadlines(void) {
    {
        spu_t *s = spu_create();
        spu_write(s, 0x1a4, 0x400); spu_write(s, 0x1aa, 0x8040);
        /* The first decode reads address zero, not the armed IRQ address.
         * Even maximum pitch cannot decode another block for seven samples. */
        assert(spu_irq_deadline(s) >= spu_next_event(s) + 7 * SPU_SAMPLE_CYCLES);
        spu_destroy(s);
    }
    for (unsigned mode = 0; mode < 12; ++mode) {
        unsigned kind = mode % 6;
        spu_t *fine = spu_create(), *batch = spu_create();
        irq_clock_t clocks[2] = {{fine, 0}, {batch, 0}};
        spu_set_irq(fine, irq_clock, &clocks[0]); spu_set_irq(batch, irq_clock, &clocks[1]);
        for (unsigned i = 0; i < 2; ++i) {
            spu_t *s = clocks[i].s;
            spu_write(s, 0x1a2, kind == 2 ? 0xff00 : 0);
            spu_write(s, 0x1a4, kind == 0 ? 1 : kind == 1 ? 0x421 : 0xff05);
            spu_write(s, 0x1ac, 4); spu_write(s, 0x1aa, 0xc0c0);
            for (unsigned r = 0; r < 32; ++r) spu_write(s, 0x1c0 + r * 2, (uint16_t)(r * 17));
        }
        if (kind >= 3) for (unsigned i = 0; i < 2; ++i) {
            spu_t *s = clocks[i].s;
            tone(s, kind == 5 ? 1 : 0, kind == 4 ? 3 : 0);
            spu_write(s, 0x1a4, kind == 4 ? 0x200 : 0x202);
            spu_write(s, (kind == 5 ? 16 : 0) + 4, 0x4000);
            if (kind == 5) spu_write(s, 0x190, 2);
            spu_write(s, 0x1aa, 0xc040);
        }
        for (uint64_t cycle = 768; cycle <= 768 * 30000; cycle += 768) {
            if (cycle % (768 * 97) == 0) {
                spu_advance(batch, cycle - 768);
                for (unsigned i = 0; i < 2; ++i) {
                    spu_t *s = clocks[i].s;
                    spu_write(s, 0x1aa, 0xc080);
                    spu_write(s, 0x1a4, (uint16_t)(cycle / 768));
                    spu_write(s, 0x1aa, 0xc0c0);
                    spu_write(s, 0x1a6, (uint16_t)(cycle / 768 + 1));
                    spu_dma_write(s, (uint32_t)cycle);
                    (void)spu_batch_deadline(s);
                    (void)spu_dma_read(s);
                }
            }
            spu_advance(fine, cycle);
            if ((mode < 6 ? spu_batch_deadline(batch) : spu_irq_deadline(batch)) <= cycle) spu_advance(batch, cycle);
            assert(clocks[0].cycle == clocks[1].cycle);
        }
        spu_advance(batch, 768 * 30000);
        assert(clocks[0].cycle != 0);
        assert(!memcmp(spu_ram(fine), spu_ram(batch), 512 * 1024));
        assert(!memcmp(fine->regs, batch->regs, sizeof(fine->regs)));
        assert(!memcmp(fine->output, batch->output, sizeof(fine->output)));
        spu_destroy(fine); spu_destroy(batch);
    }
}

static void volume_registers(void) {
    spu_t *s = spu_create();
    unsigned n, ch;
    for (n = 0; n < 24; ++n) for (ch = 0; ch < 2; ++ch) {
        unsigned setting = n * 16 + ch * 2, current = 0x200 + n * 4 + ch * 2;
        spu_write(s, setting, 0x1234);
        assert(spu_read(s, current) == 0);
    }
    tick(s, 1);
    for (n = 0; n < 24; ++n) for (ch = 0; ch < 2; ++ch) {
        unsigned setting = n * 16 + ch * 2, current = 0x200 + n * 4 + ch * 2;
        assert(spu_read(s, current) == 0x2468);
        spu_write(s, current, 0x5678);
        assert(spu_read(s, current) == 0x5678);
        spu_write(s, setting, 0x9000);
    }
    tick(s, 1);
    for (n = 0; n < 24; ++n) for (ch = 0; ch < 2; ++ch)
        assert(spu_read(s, 0x200 + n * 4 + ch * 2) == 0x1678);
    tick(s, 1);
    for (n = 0; n < 24; ++n) for (ch = 0; ch < 2; ++ch) {
        assert(spu_read(s, 0x200 + n * 4 + ch * 2) == 0xd678);
        spu_write(s, n * 16 + ch * 2, 0x3fff);
    }
    spu_write(s, 0x180, 0x1234); spu_write(s, 0x182, 0x2345);
    tick(s, 1);
    assert(spu_read(s, 0x1b8) == 0x2468 && spu_read(s, 0x1ba) == 0x468a);
    spu_write(s, 0x1b8, 1); spu_write(s, 0x1ba, 2);
    spu_write(s, 0x200, 3); spu_write(s, 0x202, 4);
    assert(spu_read(s, 0x1b8) == 1 && spu_read(s, 0x1ba) == 2);
    tick(s, 1);
    assert(spu_read(s, 0x1b8) == 0x2468 && spu_read(s, 0x1ba) == 0x468a);
    assert(spu_read(s, 0x200) == 0x7ffe && spu_read(s, 0x202) == 0x7ffe);
    spu_destroy(s);
}

int main(int argc, char **argv) {
    assert(argc == 2);
    if (!strcmp(argv[1], "deadlines")) deadlines();
    else if (!strcmp(argv[1], "adpcm")) adpcm();
    else if (!strcmp(argv[1], "gaussian")) gauss();
    else if (!strcmp(argv[1], "adsr")) adsr();
    else if (!strcmp(argv[1], "sample")) known_sample();
    else if (!strcmp(argv[1], "loops")) loops();
    else if (!strcmp(argv[1], "transfers")) transfers();
    else if (!strcmp(argv[1], "dma")) dma();
    else if (!strcmp(argv[1], "stream_dma")) stream_dma();
    else if (!strcmp(argv[1], "cd")) cd_mix();
    else if (!strcmp(argv[1], "modulation")) modulation();
    else if (!strcmp(argv[1], "reverb")) reverb();
    else if (!strcmp(argv[1], "reverb_addresses")) reverb_addresses();
    else if (!strcmp(argv[1], "volume_registers")) volume_registers();
    else return 2;
    puts("ok"); return 0;
}
