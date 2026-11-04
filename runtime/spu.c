#include "spu.h"
#include <stdlib.h>
#include <stddef.h>
#include <string.h>

#define RAM_MASK 0x7ffffu
#define AUDIO_FRAMES 16384u
#define CD_FRAMES 16384u
#define REG(s, offset) ((s)->regs[(offset) / 2])

enum { ENV_OFF, ENV_ATTACK, ENV_DECAY, ENV_SUSTAIN, ENV_RELEASE };
typedef struct { int32_t level; uint32_t counter; } envelope_t;
typedef struct {
    envelope_t adsr, volume[2];
    unsigned phase, delay, position, fraction, decoded, flags;
    uint32_t address;
    int16_t samples[31]; /* Three preceding samples followed by the current block. */
    int16_t history[2];
} voice_t;

#ifdef FF8_PLAY
typedef struct {
    uint8_t data[16]; int16_t history[2], output[28]; unsigned valid;
} decode_memo_t;
#endif
typedef struct spu_control spu_control_t;
static void control_sync(spu_t *s);
static void control_log(spu_t *s, unsigned op, uint32_t value, const void *data, unsigned bytes);
static void control_destroy(spu_t *s);
static void control_uncache(spu_t *s);

struct spu {
    uint8_t ram[512 * 1024];
    uint16_t regs[512], fifo[32];
    voice_t voices[24];
    envelope_t master[2];
    uint32_t transfer, endx, key_on, key_off, reverb_address;
    uint32_t reverb_size, reverb_offset[32], reverb_previous[4], reverb_delayed[4];
    unsigned reverb_dirty;
    uint32_t volume_work[2], volume_sweep[2];
    unsigned master_work, master_sweep;
    uint16_t status, noise;
    int32_t noise_timer; /* Remaining phase in fixed 1/0x20000 clock units. */
    unsigned fifo_count, capture, fir_position, reverb_tail;
    int16_t reverb_input[2][64], reverb_output[2][64];
    int16_t output[AUDIO_FRAMES][2], cd[CD_FRAMES][2];
    unsigned output_read, output_count, cd_read, cd_count;
    uint64_t next_sample, frames, dropped, cd_dropped, cd_received, key_ons, blocks, dma_words;
    uint64_t voice_frames, cd_audible_frames;
    uint64_t batch_sample, batch_due, irq_sample, irq_due;
    unsigned batch_valid, irq_valid;
    uint64_t voice_refresh, voice_due, voice_irq_due;
    uint64_t fetch_due[24], fetch_irq_due[24];
    unsigned voice_valid, voice_cached;
    void (*irq)(void *);
    void *irq_context;
    const spu_backend_t *backend;
    void *backend_context;
    spu_control_t *control;
    unsigned replay, bulk;
    uint64_t allocation_fallback_calls, allocation_fallback_frames;
    uint64_t control_frames, bulk_frames, interpolations;
#ifdef FF8_PLAY
    decode_memo_t decode_memo[24];
    uint64_t live_block_fetches;
#ifdef FF8_SPU_CACHE_TEST
    unsigned decode_hits, decode_skipped;
#endif
#endif
};

/* Hardware coefficients, psx-spx Sound Processing Unit / 4-Point Gaussian
 * Interpolation. These are the PS1 ROM values, not an analytic approximation.
 * https://psx-spx.consoledev.net/soundprocessingunitspu/ */
static const int16_t gaussian[512] = {
    -1,-1,-1,-1,-1,-1,-1,-1, -1,-1,-1,-1,-1,-1,-1,-1,
    0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0001,
    0x0001,0x0001,0x0001,0x0002,0x0002,0x0002,0x0003,0x0003,
    0x0003,0x0004,0x0004,0x0005,0x0005,0x0006,0x0007,0x0007,
    0x0008,0x0009,0x0009,0x000a,0x000b,0x000c,0x000d,0x000e,
    0x000f,0x0010,0x0011,0x0012,0x0013,0x0015,0x0016,0x0018,
    0x0019,0x001b,0x001c,0x001e,0x0020,0x0021,0x0023,0x0025,
    0x0027,0x0029,0x002c,0x002e,0x0030,0x0033,0x0035,0x0038,
    0x003a,0x003d,0x0040,0x0043,0x0046,0x0049,0x004d,0x0050,
    0x0054,0x0057,0x005b,0x005f,0x0063,0x0067,0x006b,0x006f,
    0x0074,0x0078,0x007d,0x0082,0x0087,0x008c,0x0091,0x0096,
    0x009c,0x00a1,0x00a7,0x00ad,0x00b3,0x00ba,0x00c0,0x00c7,
    0x00cd,0x00d4,0x00db,0x00e3,0x00ea,0x00f2,0x00fa,0x0101,
    0x010a,0x0112,0x011b,0x0123,0x012c,0x0135,0x013f,0x0148,
    0x0152,0x015c,0x0166,0x0171,0x017b,0x0186,0x0191,0x019c,
    0x01a8,0x01b4,0x01c0,0x01cc,0x01d9,0x01e5,0x01f2,0x0200,
    0x020d,0x021b,0x0229,0x0237,0x0246,0x0255,0x0264,0x0273,
    0x0283,0x0293,0x02a3,0x02b4,0x02c4,0x02d6,0x02e7,0x02f9,
    0x030b,0x031d,0x0330,0x0343,0x0356,0x036a,0x037e,0x0392,
    0x03a7,0x03bc,0x03d1,0x03e7,0x03fc,0x0413,0x042a,0x0441,
    0x0458,0x0470,0x0488,0x04a0,0x04b9,0x04d2,0x04ec,0x0506,
    0x0520,0x053b,0x0556,0x0572,0x058e,0x05aa,0x05c7,0x05e4,
    0x0601,0x061f,0x063e,0x065c,0x067c,0x069b,0x06bb,0x06dc,
    0x06fd,0x071e,0x0740,0x0762,0x0784,0x07a7,0x07cb,0x07ef,
    0x0813,0x0838,0x085d,0x0883,0x08a9,0x08d0,0x08f7,0x091e,
    0x0946,0x096f,0x0998,0x09c1,0x09eb,0x0a16,0x0a40,0x0a6c,
    0x0a98,0x0ac4,0x0af1,0x0b1e,0x0b4c,0x0b7a,0x0ba9,0x0bd8,
    0x0c07,0x0c38,0x0c68,0x0c99,0x0ccb,0x0cfd,0x0d30,0x0d63,
    0x0d97,0x0dcb,0x0e00,0x0e35,0x0e6b,0x0ea1,0x0ed7,0x0f0f,
    0x0f46,0x0f7f,0x0fb7,0x0ff1,0x102a,0x1065,0x109f,0x10db,
    0x1116,0x1153,0x118f,0x11cd,0x120b,0x1249,0x1288,0x12c7,
    0x1307,0x1347,0x1388,0x13c9,0x140b,0x144d,0x1490,0x14d4,
    0x1517,0x155c,0x15a0,0x15e6,0x162c,0x1672,0x16b9,0x1700,
    0x1747,0x1790,0x17d8,0x1821,0x186b,0x18b5,0x1900,0x194b,
    0x1996,0x19e2,0x1a2e,0x1a7b,0x1ac8,0x1b16,0x1b64,0x1bb3,
    0x1c02,0x1c51,0x1ca1,0x1cf1,0x1d42,0x1d93,0x1de5,0x1e37,
    0x1e89,0x1edc,0x1f2f,0x1f82,0x1fd6,0x202a,0x207f,0x20d4,
    0x2129,0x217f,0x21d5,0x222c,0x2282,0x22da,0x2331,0x2389,
    0x23e1,0x2439,0x2492,0x24eb,0x2545,0x259e,0x25f8,0x2653,
    0x26ad,0x2708,0x2763,0x27be,0x281a,0x2876,0x28d2,0x292e,
    0x298b,0x29e7,0x2a44,0x2aa1,0x2aff,0x2b5c,0x2bba,0x2c18,
    0x2c76,0x2cd4,0x2d33,0x2d91,0x2df0,0x2e4f,0x2eae,0x2f0d,
    0x2f6c,0x2fcc,0x302b,0x308b,0x30ea,0x314a,0x31aa,0x3209,
    0x3269,0x32c9,0x3329,0x3389,0x33e9,0x3449,0x34a9,0x3509,
    0x3569,0x35c9,0x3629,0x3689,0x36e8,0x3748,0x37a8,0x3807,
    0x3867,0x38c6,0x3926,0x3985,0x39e4,0x3a43,0x3aa2,0x3b00,
    0x3b5f,0x3bbd,0x3c1b,0x3c79,0x3cd7,0x3d35,0x3d92,0x3def,
    0x3e4c,0x3ea9,0x3f05,0x3f62,0x3fbd,0x4019,0x4074,0x40d0,
    0x412a,0x4185,0x41df,0x4239,0x4292,0x42eb,0x4344,0x439c,
    0x43f4,0x444c,0x44a3,0x44fa,0x4550,0x45a6,0x45fc,0x4651,
    0x46a6,0x46fa,0x474e,0x47a1,0x47f4,0x4846,0x4898,0x48e9,
    0x493a,0x498a,0x49d9,0x4a29,0x4a77,0x4ac5,0x4b13,0x4b5f,
    0x4bac,0x4bf7,0x4c42,0x4c8d,0x4cd7,0x4d20,0x4d68,0x4db0,
    0x4df7,0x4e3e,0x4e84,0x4ec9,0x4f0e,0x4f52,0x4f95,0x4fd7,
    0x5019,0x505a,0x509a,0x50da,0x5118,0x5156,0x5194,0x51d0,
    0x520c,0x5247,0x5281,0x52ba,0x52f3,0x532a,0x5361,0x5397,
    0x53cc,0x5401,0x5434,0x5467,0x5499,0x54ca,0x54fa,0x5529,
    0x5558,0x5585,0x55b2,0x55de,0x5609,0x5632,0x565b,0x5684,
    0x56ab,0x56d1,0x56f6,0x571b,0x573e,0x5761,0x5782,0x57a3,
    0x57c3,0x57e2,0x57ff,0x581c,0x5838,0x5853,0x586d,0x5886,
    0x589e,0x58b5,0x58cb,0x58e0,0x58f4,0x5907,0x5919,0x592a,
    0x593a,0x5949,0x5958,0x5965,0x5971,0x597c,0x5986,0x598f,
    0x5997,0x599e,0x59a4,0x59a9,0x59ad,0x59b0,0x59b2,0x59b3
};

static int16_t clip(int32_t x) { return x < -32768 ? -32768 : x > 32767 ? 32767 : (int16_t)x; }
/* b is PCM16, or the unsigned 16-bit pitch modulation factor.
 * The widest input is the reverb IIR
 * difference (three PCM16 values). Split at the binary point so both products
 * fit int32_t and no wide multiply helper is needed, including negative rounding. */
static int32_t mul(int32_t a, int32_t b) {
    return (a >> 15) * b + (((a & 0x7fff) * b) >> 15);
}
/* PCM products fit int32_t. The reverb IIR difference uses mul above. */
static int32_t mul_sample(int32_t a, int32_t b) { return (a * b) >> 15; }
static uint32_t flags(const spu_t *s, unsigned offset) { return REG(s, offset) | ((uint32_t)(REG(s, offset + 2) & 255) << 16); }

#ifdef FF8_SPU_TRACE
/* Separate from the binary API trace: headers and jumps identify loop damage.
 * Sample bounds keep real-disc captures small; no trace state is serialized. */
static void trace_voice(const spu_t *s, unsigned n, const char *event, uint32_t value) {
    static FILE *out;
    static unsigned initialized;
    static uint64_t first, last = UINT64_MAX;
    const voice_t *v = &s->voices[n];
    if (!initialized) {
        const char *path = getenv("FF8_SPU_DIAGNOSTICS"), *bound;
        initialized = 1;
        if (path && !(out = fopen(path, "w"))) abort();
        if ((bound = getenv("FF8_SPU_TRACE_FROM"))) first = strtoull(bound, NULL, 0);
        if ((bound = getenv("FF8_SPU_TRACE_UNTIL"))) last = strtoull(bound, NULL, 0);
    }
    if (!out || s->replay || s->frames < first || s->frames > last) return;
    fprintf(out, "SPU-TRACE sample=%llu voice=%u event=%s value=%05x on=%u pending=%u delay=%u address=%05x repeat=%05x pitch=%04x pmon=%u noise=%u adsr=%04x:%04x phase=%u env=%d flags=%02x control=%04x noise-level=%04x noise-timer=%d non=%06x pmon-mask=%06x\n",
        (unsigned long long)s->frames, n, event, value, v->phase != ENV_OFF,
        !!(s->key_on & (1u << n)), v->delay, v->address, REG(s,n*16+14)*8,
        REG(s,n*16+4), !!(flags(s,0x190)&(1u<<n)), !!(flags(s,0x194)&(1u<<n)),
        REG(s,n*16+8), REG(s,n*16+10), v->phase, v->adsr.level, v->flags,
        REG(s,0x1aa), s->noise, s->noise_timer, flags(s,0x194), flags(s,0x190));
    fflush(out);
}
#else
#define trace_voice(s, n, event, value) ((void)0)
#endif

/* psx-spx SPU Noise Generator: one LFSR update at most per output sample.
 * Express its variable reload period in fixed clock units. A frequency write
 * then changes the increment immediately without inheriting a countdown in
 * units of the old period. The 17-bit phase survives KON and frequency writes.
 * https://psx-spx.consoledev.net/soundprocessingunitspu/#spu-noise-generator */
static void noise_advance(spu_t *s, unsigned count) {
    unsigned control = REG(s,0x1aa), step = (4u + ((control >> 8) & 3)) << ((control >> 10) & 15);
    unsigned total = step * count, timer = (unsigned)s->noise_timer, updates;
    if (timer >= total) { s->noise_timer -= (int32_t)total; return; }
    updates = step <= 0x20000u ? (total - timer + 0x1ffffu) / 0x20000u : count;
    s->noise_timer = (int32_t)((timer - total) & 0x1ffffu);
    while (updates--) {
        unsigned bit = ((s->noise >> 15) ^ (s->noise >> 12) ^ (s->noise >> 11) ^ (s->noise >> 10) ^ 1) & 1;
        s->noise = (uint16_t)(s->noise * 2 + bit);
    }
}

static void ram_irq(spu_t *s, uint32_t address) {
    if ((REG(s, 0x1aa) & 0x8040) == 0x8040 && !(s->status & 0x40) &&
        (address & RAM_MASK) == (uint32_t)REG(s, 0x1a4) * 8) {
        s->status |= 0x40;
        if (s->irq) s->irq(s->irq_context);
    }
}
static uint16_t spu_ram_read(spu_t *s, uint32_t address) {
    address &= RAM_MASK & ~1u;
    ram_irq(s, address);
    return s->ram[address] | ((uint16_t)s->ram[address + 1] << 8);
}
static void spu_ram_write(spu_t *s, uint32_t address, uint16_t value) {
    control_uncache(s);
    address &= RAM_MASK & ~1u;
    ram_irq(s, address);
    s->ram[address] = (uint8_t)value; s->ram[address + 1] = (uint8_t)(value >> 8);
}

static void decode_block(const uint8_t data[16], int16_t history[2], int16_t out[28]) {
    static const int filter[5][2] = {{0,0},{60,0},{115,-52},{98,-55},{122,-60}};
    static const uint8_t zero[14] = {0};
    unsigned shift = data[0] & 15, predictor = data[0] >> 4, i;
    if (shift > 12) shift = 9;
    if (predictor > 4) predictor = 0;
    if ((!predictor || (!history[0] && !history[1])) && !memcmp(data + 2, zero, sizeof(zero))) {
        memset(out, 0, 28 * sizeof(*out));
        history[0] = history[1] = 0;
        return;
    }
    if (!predictor) {
        /* Signed nibble * 4096, identical to the sign-extended byte forms. */
        static const int32_t nibble[16] = {0, 4096, 8192, 12288, 16384, 20480, 24576, 28672,
                                           -32768, -28672, -24576, -20480, -16384, -12288, -8192, -4096};
        for (i = 0; i < 14; ++i) {
            unsigned byte = data[2 + i];
            out[i * 2] = nibble[byte & 15] >> shift;
            out[i * 2 + 1] = nibble[byte >> 4] >> shift;
        }
        history[0] = out[27]; history[1] = out[26];
        return;
    }
    for (i = 0; i < 28; ++i) {
        int32_t nibble = (data[2 + i / 2] >> ((i & 1) * 4)) & 15;
        int32_t sample;
        if (nibble & 8) nibble -= 16;
        sample = (nibble * 4096) >> shift;
        sample += (history[0] * filter[predictor][0] + history[1] * filter[predictor][1] + 32) >> 6;
        history[1] = history[0]; history[0] = out[i] = clip(sample);
    }
}

#ifdef FF8_PLAY
static inline int decode_bytes_equal(const uint8_t *left, const uint8_t *right) {
    uint32_t a[4], b[4];
    memcpy(a, left, sizeof(a)); memcpy(b, right, sizeof(b));
    return a[0] == b[0] && a[1] == b[1] && a[2] == b[2] && a[3] == b[3];
}
static void decode_cached(spu_t *s, unsigned n, const uint8_t data[16]) {
    voice_t *v = &s->voices[n]; decode_memo_t *m = &s->decode_memo[n];
    unsigned predictor = data[0] >> 4;
    if (m->valid && decode_bytes_equal(m->data, data) &&
        (!predictor || predictor > 4 || (m->history[0] == v->history[0] && m->history[1] == v->history[1]))) {
        memcpy(v->samples + 3, m->output, sizeof(m->output));
        v->history[0] = m->output[27]; v->history[1] = m->output[26];
#ifdef FF8_SPU_CACHE_TEST
        ++s->decode_hits;
#endif
        return;
    }
    m->valid = 1; memcpy(m->data, data, sizeof(m->data));
    memcpy(m->history, v->history, sizeof(m->history));
    decode_block(data, v->history, v->samples + 3);
    memcpy(m->output, v->samples + 3, sizeof(m->output));
}
#endif

/* The counter formulation matters for shifts >=27 and the all-ones infinite
 * rates. A simple samples-per-step timer gives incorrect ADSR/sweep timing. */
static int envelope_tick(envelope_t *e, unsigned shift, unsigned step_bits,
                         int exponential, int decrease, int negative, int infinite) {
    int32_t step = 7 - (int32_t)step_bits;
    uint32_t increment = 0x8000u >> (shift > 11 ? shift - 11 : 0);
    if (decrease ^ negative) step = ~step;
    step *= 1 << (shift < 11 ? 11 - shift : 0);
    if (exponential && !decrease && e->level > 0x6000) {
        if (shift < 10) step >>= 2;
        else if (shift >= 11) increment >>= 2;
        else { step >>= 1; increment >>= 1; }
    } else if (exponential && decrease) step = mul_sample(step, e->level);
    if (!increment && !infinite) increment = 1;
    e->counter += increment;
    if (!(e->counter & 0x8000)) return 0;
    e->counter = 0;
    e->level += step;
    if (!decrease) e->level = clip(e->level);
    else if (negative) e->level = e->level > 0 ? 0 : clip(e->level);
    else if (e->level < 0) e->level = 0;
    return 1;
}

static void adsr_tick(voice_t *v, uint16_t low, uint16_t high) {
    unsigned shift, step = 0;
    int exponential, decrease, infinite = 0, target = 0;
    switch (v->phase) {
    case ENV_ATTACK:
        shift = (low >> 10) & 31; step = (low >> 8) & 3;
        exponential = low >> 15; decrease = 0; target = 32767;
        infinite = (low & 0x7f00) == 0x7f00; break;
    case ENV_DECAY:
        shift = (low >> 4) & 15; exponential = 1; decrease = 1;
        target = ((low & 15) + 1) * 0x800; break;
    case ENV_SUSTAIN:
        shift = (high >> 8) & 31; step = (high >> 6) & 3;
        exponential = high >> 15; decrease = (high >> 14) & 1;
        infinite = (high & 0x1fc0) == 0x1fc0; break;
    case ENV_RELEASE:
        shift = high & 31; exponential = (high >> 5) & 1; decrease = 1;
        infinite = shift == 31; break;
    default: return;
    }
    if (!envelope_tick(&v->adsr, shift, step, exponential, decrease, 0, infinite)) return;
    if ((v->phase == ENV_ATTACK && v->adsr.level >= target) ||
        (v->phase == ENV_DECAY && v->adsr.level <= target)) {
        ++v->phase; v->adsr.counter = 0;
    } else if (v->phase == ENV_RELEASE && !v->adsr.level) v->phase = ENV_OFF;
}

static void volume_tick(envelope_t *v, uint16_t setting) {
    if (!(setting & 0x8000)) { v->level = (int16_t)(setting * 2); v->counter = 0; }
    else {
        int decrease = !!(setting & 0x2000), exponential = !!(setting & 0x4000);
        int negative = !!(setting & 0x1000) && !(exponential && decrease);
        envelope_tick(v, (setting >> 2) & 31, setting & 3, exponential, decrease,
                      negative, (setting & 127) == 127);
    }
}

static void start_voice(spu_t *s, unsigned n) {
    voice_t *v = &s->voices[n];
    v->phase = ENV_ATTACK; v->adsr.level = 0; v->adsr.counter = 0;
    v->address = ((uint32_t)REG(s, n * 16 + 6) * 8) & ~15u;
    v->position = v->fraction = v->decoded = v->flags = 0;
    v->delay = 2;
    memset(v->history, 0, sizeof(v->history)); memset(v->samples, 0, sizeof(v->samples));
    s->endx &= ~(1u << n); ++s->key_ons;
    trace_voice(s, n, "start", v->address);
}

static void voice_decode(spu_t *s, unsigned n) {
    voice_t *v = &s->voices[n];
    uint8_t data[16];
    unsigned i;
    if (!(v->address & 15) && v->address <= RAM_MASK - 15) {
        /* One IRQ comparison covers the block's eight halfword reads: the IRQ
         * address is a multiple of eight and fires at most once per enable. */
        uint32_t irq = (uint32_t)REG(s, 0x1a4) * 8;
        if (irq - v->address < 16) ram_irq(s, irq);
        memcpy(data, s->ram + v->address, sizeof(data));
    } else for (i = 0; i < 16; i += 2) {
        uint16_t word = spu_ram_read(s, v->address + i);
        data[i] = (uint8_t)word; data[i + 1] = (uint8_t)(word >> 8);
    }
    memcpy(v->samples, v->samples + 28, 3 * sizeof(int16_t));
#ifdef FF8_PLAY
    if (s->control && !s->replay) ++s->live_block_fetches;
    decode_cached(s, n, data);
#else
    decode_block(data, v->history, v->samples + 3);
#endif
    v->flags = data[1]; v->decoded = 1; ++s->blocks;
    if (v->flags & 4) {
        if (v->phase) trace_voice(s, n, "loop-start", v->address);
        REG(s, n * 16 + 14) = (uint16_t)(v->address / 8);
    }
}

static int16_t interpolate(const int16_t *samples, unsigned fraction) {
    unsigned i = (fraction >> 4) & 255;
    int32_t out = mul_sample(gaussian[255 - i], samples[0]) + mul_sample(gaussian[511 - i], samples[1]) +
                  mul_sample(gaussian[256 + i], samples[2]) + mul_sample(gaussian[i], samples[3]);
    return clip(out);
}

static int32_t voice_sample_needed(spu_t *s, unsigned n, int32_t previous, uint32_t pmon, uint32_t noise, int needed) {
    voice_t *v = &s->voices[n];
    uint16_t *r = &REG(s, n * 16);
    uint32_t step = r[2];
    int32_t out;
    if (v->delay) { --v->delay; return 0; }
    if (!v->decoded) voice_decode(s, n);
    if (needed && v->adsr.level && !(noise & (1u << n))) ++s->interpolations;
    out = needed && v->adsr.level ? mul_sample((noise & (1u << n)) ? (int16_t)s->noise :
        interpolate(v->samples + v->position, v->fraction), v->adsr.level) : 0;
    adsr_tick(v, r[4], r[5]);
    if (n && (pmon & (1u << n))) step = (uint16_t)mul_sample((int16_t)step, previous + 32768);
    if (step > 0x4000) step = 0x4000;
    v->fraction += step;
    v->position += v->fraction >> 12; v->fraction &= 0xfff;
    if (v->position >= 28) {
        v->position -= 28; v->decoded = 0;
        if (v->flags & 1) {
            s->endx |= 1u << n;
            if (v->phase) trace_voice(s, n, "loop-end", (uint32_t)r[7] * 8 & ~15u);
            v->address = ((uint32_t)r[7] * 8) & ~15u;
            if (!(v->flags & 2) && !(noise & (1u << n))) { v->phase = ENV_OFF; v->adsr.level = 0; }
        } else v->address = (v->address + 16) & RAM_MASK;
    }
    return out;
}

/* Kept as a small wrapper for the full-model numeric harness. */
static int32_t voice_sample(spu_t *s, unsigned n, int32_t previous, uint32_t pmon, uint32_t noise) {
    return voice_sample_needed(s, n, previous, pmon, noise, 1);
}

static uint32_t reverb_address(const spu_t *s, int32_t offset) {
    int32_t base = (int32_t)REG(s, 0x1a2) * 8, size = 0x80000 - base;
    int32_t relative = ((int32_t)s->reverb_address - base + offset) % size;
    if (relative < 0) relative += size;
    return (uint32_t)(base + relative);
}
static void reverb_offsets(spu_t *s) {
    const uint16_t *r = &REG(s, 0x1c0);
    unsigned i;
    uint32_t size = 0x80000 - (uint32_t)REG(s, 0x1a2) * 8;
    s->reverb_size = size;
    for (i = 0; i < 32; ++i) s->reverb_offset[i] = (uint32_t)r[i] * 8 % size;
    for (i = 0; i < 4; ++i) {
        uint32_t previous = s->reverb_offset[(i / 2 ? 18 : 10) + (i & 1)] + size - 2;
        uint32_t delayed = s->reverb_offset[26 + i] + size - s->reverb_offset[i / 2];
        s->reverb_previous[i] = previous >= size ? previous - size : previous;
        s->reverb_delayed[i] = delayed >= size ? delayed - size : delayed;
    }
    s->reverb_dirty = 0;
}
static uint32_t reverb_cached_address(const spu_t *s, uint32_t offset) {
    uint32_t address = s->reverb_address + offset;
    return address > RAM_MASK ? address - s->reverb_size : address;
}
static int16_t reverb_read(spu_t *s, uint32_t offset) { return (int16_t)spu_ram_read(s, reverb_cached_address(s, offset)); }
static void reverb_write(spu_t *s, uint32_t offset, int32_t value) { spu_ram_write(s, reverb_cached_address(s, offset), (uint16_t)clip(value)); }

static void reverb_core(spu_t *s, const int16_t input[2], int16_t output[2]) {
    uint16_t *r = &REG(s, 0x1c0);
    unsigned ch, i;
    int enabled = !!(REG(s, 0x1aa) & 0x80);
    if (s->reverb_dirty) reverb_offsets(s);
    if (s->backend && !enabled) {
        /* Disabled effects still touch the comb/all-pass addresses for IRQs,
         * but their values reach only the omitted output FIR. */
        for (ch = 0; ch < 2; ++ch) {
            static const unsigned comb[4] = {12,14,20,22};
            for (i = 0; i < 4; ++i) (void)reverb_read(s, s->reverb_offset[comb[i] + ch]);
            for (i = 0; i < 2; ++i) (void)reverb_read(s, s->reverb_delayed[i * 2 + ch]);
        }
        s->reverb_address += 2;
        if (s->reverb_address > RAM_MASK) s->reverb_address = (uint32_t)REG(s, 0x1a2) * 8;
        return;
    }
    for (ch = 0; ch < 2; ++ch) {
        int32_t value = 0;
        if (enabled) {
            int32_t in = mul_sample(input[ch], (int16_t)r[30 + ch]);
            for (i = 0; i < 2; ++i) {
                unsigned dest = (i ? 18 : 10) + ch, source = i ? 24 + (ch ^ 1) : 16 + ch;
                int32_t old = reverb_read(s, s->reverb_previous[i * 2 + ch]);
                int32_t reflection = in + mul_sample(reverb_read(s, s->reverb_offset[source]), (int16_t)r[7]);
                int32_t filtered = mul(reflection - old, (int16_t)r[2]) + old;
                if (r[2] == 0x8000) filtered = -filtered;
                reverb_write(s, s->reverb_offset[dest], filtered);
            }
        }
        for (i = 0; i < 4; ++i) {
            static const unsigned comb[4] = {12,14,20,22};
            value += mul_sample(reverb_read(s, s->reverb_offset[comb[i] + ch]), (int16_t)r[3 + i]);
        }
        for (i = 0; i < 2; ++i) {
            unsigned dest = 26 + i * 2 + ch;
            int32_t delayed = reverb_read(s, s->reverb_delayed[i * 2 + ch]);
            int32_t apf = clip(value - mul_sample(delayed, (int16_t)r[8 + i]));
            if (enabled) reverb_write(s, s->reverb_offset[dest], apf);
            value = clip(mul_sample(apf, (int16_t)r[8 + i]) + delayed);
        }
        output[ch] = clip(value);
    }
    s->reverb_address += 2;
    if (s->reverb_address > RAM_MASK) s->reverb_address = (uint32_t)REG(s, 0x1a2) * 8;
}

static const int16_t reverb_fir[39] = {
    -1,0,2,0,-10,0,35,0,-103,0,266,0,-616,0,1332,0,-2960,0,10246,16384,
    10246,0,-2960,0,1332,0,-616,0,266,0,-103,0,35,0,-10,0,2,0,-1
};
static int16_t fir(const int16_t *buffer, unsigned pos, unsigned gain) {
    /* Sum of absolute coefficients is 47526: PCM16 accumulation fits int32. */
    int32_t total = (int32_t)buffer[(pos - 19) & 63] * reverb_fir[19];
    unsigned i;
    for (i = 0; i < 39; i += 2) total += (int32_t)buffer[(pos - i) & 63] * reverb_fir[i];
    return clip(total >> (gain == 2 ? 14 : 15));
}
static void reverb_sample(spu_t *s, const int32_t input[2], int32_t output[2]) {
    int16_t down[2], up[2] = {0,0};
    unsigned ch, pos = s->fir_position;
    for (ch = 0; ch < 2; ++ch) s->reverb_input[ch][pos] = clip(input[ch]);
    if (!(pos & 1) && (REG(s, 0x1ac) & 12)) {
        for (ch = 0; ch < 2; ++ch) down[ch] = s->backend && !(REG(s, 0x1aa) & 0x80) ? 0 : fir(s->reverb_input[ch], pos, 1);
        reverb_core(s, down, up);
    }
    /* Output FIR has no path back to registers, IRQs, or RAM. The input FIR
     * and core RAM effects above remain exact, including while muted. */
    if (s->backend) { s->fir_position = (pos + 1) & 63; return; }
    if (up[0] || up[1]) s->reverb_tail = 64;
    for (ch = 0; ch < 2; ++ch) {
        s->reverb_output[ch][pos] = up[ch];
        output[ch] = s->reverb_tail ? mul_sample(fir(s->reverb_output[ch], pos, 2), (int16_t)REG(s, 0x184 + ch * 2)) : 0;
    }
    if (s->reverb_tail) --s->reverb_tail;
    s->fir_position = (pos + 1) & 63;
}

static void manual_transfer(spu_t *s) {
    unsigned i;
    for (i = 0; i < s->fifo_count; ++i) {
        spu_ram_write(s, s->transfer, s->fifo[i]);
        if (s->backend && s->backend->upload)
            s->backend->upload(s->backend_context, s->transfer, s->ram + s->transfer, 2);
        s->transfer = (s->transfer + 2) & RAM_MASK;
    }
    s->fifo_count = 0;
}

#ifdef FF8_PLAY
static void sample_active(spu_t *s, const uint8_t *active, unsigned active_count) {
    unsigned active_index, previous_index = ~0u;
#else
static void sample(spu_t *s) {
#endif
    unsigned n, ch, control = REG(s, 0x1aa), transfer_mode = (control >> 4) & 3;
    uint32_t pmon = flags(s, 0x190), noise = flags(s, 0x194), echo = flags(s, 0x198);
    int32_t mix[2] = {0,0}, reverb_in[2] = {0,0}, wet[2] = {0,0}, previous = 0, capture[2] = {0,0};
    uint32_t needed = (pmon >> 1) | ((REG(s, 0x1ac) & 12) ? 0xau : 0) |
        ((control & 0x4000) ? echo : 0);
    int16_t cd[2] = {0,0};
    if (transfer_mode == 1) manual_transfer(s);
    if (s->backend && s->backend->write) {
        /* The reference latches both masks until a sample, applying KOFF
         * before KON even if MMIO writes arrived in the opposite order. */
        if (s->key_off & 0xffff) s->backend->write(s->backend_context, 0x18c, (uint16_t)s->key_off);
        if (s->key_off >> 16) s->backend->write(s->backend_context, 0x18e, (uint16_t)(s->key_off >> 16));
        if ((control & 0x8000) && (s->key_on & 0xffff)) s->backend->write(s->backend_context, 0x188, (uint16_t)s->key_on);
        if ((control & 0x8000) && (s->key_on >> 16)) s->backend->write(s->backend_context, 0x18a, (uint16_t)(s->key_on >> 16));
    }
    s->status = (s->status & 0x40) | (control & 0x3f) |
        (transfer_mode >= 2 ? 0x80 | (transfer_mode == 2 ? 0x100 : 0x200) : 0);
    noise_advance(s, 1);
#ifdef FF8_PLAY
    for (active_index = 0; active_index < active_count; ++active_index) {
        n = active[active_index];
        if (n != previous_index + 1u) previous = 0;
        previous_index = n;
#else
    for (n = 0; n < 24; ++n) {
        if (s->bulk & (1u << n)) { previous = 0; continue; }
#endif
        voice_t *v = &s->voices[n];
        if (s->key_off & (1u << n)) { v->phase = ENV_RELEASE; v->adsr.counter = 0; }
        if (s->key_on & (1u << n)) start_voice(s, n);
        if (!(control & 0x8000)) { v->phase = ENV_OFF; v->adsr.level = 0; }
        previous = s->backend ? voice_sample_needed(s, n, previous, pmon, noise, !!(needed & (1u << n))) :
            voice_sample(s, n, previous, pmon, noise);
        if (n == 1 || n == 3) capture[n / 2] = previous;
        if (!previous && !((s->volume_work[0] | s->volume_work[1]) & (1u << n))) continue;
        for (ch = 0; ch < 2; ++ch) {
            int32_t value;
            if (s->volume_work[ch] & (1u << n))
                volume_tick(&v->volume[ch], REG(s, n * 16 + ch * 2));
            if (!previous || (s->backend && !(echo & (1u << n)))) continue;
            value = mul_sample(previous, v->volume[ch].level);
            if (!s->backend) mix[ch] += value;
            if (echo & (1u << n)) reverb_in[ch] += value;
        }
    }
    s->volume_work[0] = s->volume_sweep[0]; s->volume_work[1] = s->volume_sweep[1];
    s->key_on = s->key_off = 0;
    if (!(control & 0x4000)) { mix[0] = mix[1] = 0; reverb_in[0] = reverb_in[1] = 0; }
    if (mix[0] || mix[1]) ++s->voice_frames;
    if (s->cd_count) {
        cd[0] = s->cd[s->cd_read][0]; cd[1] = s->cd[s->cd_read][1];
        s->cd_read = (s->cd_read + 1) % CD_FRAMES; --s->cd_count;
    }
    for (ch = 0; ch < 2; ++ch) {
        int32_t value = mul_sample(cd[ch], (int16_t)REG(s, 0x1b0 + ch * 2));
        if (control & 1) { mix[ch] += value; if (control & 4) reverb_in[ch] += value; }
        if (REG(s, 0x1ac) & 12) {
            spu_ram_write(s, s->capture + ch * 0x400, (uint16_t)cd[ch]);
            spu_ram_write(s, s->capture + 0x800 + ch * 0x400, (uint16_t)capture[ch]);
        }
    }
    if ((control & 1) && (mul_sample(cd[0], (int16_t)REG(s, 0x1b0)) || mul_sample(cd[1], (int16_t)REG(s, 0x1b2))))
        ++s->cd_audible_frames;
    if (REG(s, 0x1ac) & 12) {
        s->capture = (s->capture + 2) & 0x3ff;
        if (s->capture & 0x200) s->status |= 0x800;
    }
    reverb_sample(s, reverb_in, wet);
    if (s->output_count == AUDIO_FRAMES) {
        s->output_read = (s->output_read + 1) % AUDIO_FRAMES; --s->output_count; ++s->dropped;
    }
    for (ch = 0; ch < 2; ++ch) {
        if (s->master_work & (1u << ch)) volume_tick(&s->master[ch], REG(s, 0x180 + ch * 2));
        s->output[(s->output_read + s->output_count) % AUDIO_FRAMES][ch] =
            s->backend ? cd[ch] : clip(mul_sample(clip(mix[ch] + wet[ch]), s->master[ch].level));
    }
    s->master_work = s->master_sweep;
    ++s->output_count; ++s->frames;
    if (s->backend) ++s->control_frames;
}

spu_t *spu_create(void) {
    spu_t *s = calloc(1, sizeof(*s));
    if (s) { s->next_sample = SPU_SAMPLE_CYCLES; s->reverb_dirty = 1; }
    return s;
}
void spu_destroy(spu_t *s) { if (s) control_destroy(s); free(s); }
void spu_set_irq(spu_t *s, void (*raise)(void *), void *context) { s->irq = raise; s->irq_context = context; }
static void control_create(spu_t *s);
#ifdef FF8_PLAY
static void sample(spu_t *s) {
    static const uint8_t all[24] = {0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23};
    uint8_t active[24]; unsigned n, count = 0;
    if (!s->bulk) { sample_active(s, all, 24); return; }
    for (n = 0; n < 24; ++n) if (!(s->bulk & (1u << n))) active[count++] = (uint8_t)n;
    sample_active(s, active, count);
}
#endif

void spu_set_backend(spu_t *s, const spu_backend_t *backend, void *context) {
    /* Mode changes after synthesis would need the omitted output FIR history. */
    if (s->frames) return;
    if (!backend && s->control) { control_sync(s); control_destroy(s); s->control = NULL; }
    s->backend = backend; s->backend_context = context;
    if (backend && !s->replay) control_create(s);
}
uint64_t spu_next_event(const spu_t *s) { return s->next_sample; }
static unsigned reverb_irq_delay(const spu_t *s, int32_t offset) {
    uint32_t base = (uint32_t)REG(s, 0x1a2) * 8, size = 0x80000 - base;
    uint32_t address = (uint32_t)REG(s, 0x1a4) * 8;
    uint32_t current;
    if (address < base) return UINT32_MAX;
    current = reverb_address(s, offset);
    return (address + size - current) % size + (s->fir_position & 1);
}

static void voice_deadlines(spu_t *s) {
    uint32_t pmon = flags(s, 0x190), address = (uint32_t)REG(s, 0x1a4) * 8;
    unsigned n;
    /* With fixed pitch, the next fetch has one absolute date. Its address and
     * flags cannot change before that fetch. PMON keeps the conservative scan
     * because the maximum pitch is only a bound on its changing live pitch. */
    if (s->voice_valid && !pmon && s->next_sample <= s->voice_refresh) return;
    s->voice_due = s->voice_irq_due = s->voice_refresh = UINT64_MAX;
    for (n = 0; n < 24; ++n) {
        const voice_t *v = &s->voices[n];
        unsigned step = REG(s, n * 16 + 4), delay;
        uint32_t next;
        uint64_t due, irq_due;
        if (!pmon && (s->voice_cached & (1u << n)) && s->next_sample <= s->fetch_due[n]) {
            due = s->fetch_due[n]; irq_due = s->fetch_irq_due[n];
        } else {
            if (n && (pmon & (1u << n))) step = 0x4000;
            if (step > 0x4000) step = 0x4000;
            if (!v->decoded) delay = v->delay;
            else if (!step) {
                s->fetch_due[n] = s->fetch_irq_due[n] = UINT64_MAX;
                s->voice_cached |= 1u << n; continue;
            }
            else delay = v->delay + ((28 - v->position) * 4096 - v->fraction + step - 1) / step;
            due = s->next_sample + (uint64_t)delay * SPU_SAMPLE_CYCLES;
            next = !v->decoded ? v->address : v->flags & 1 ?
                ((uint32_t)REG(s, n * 16 + 14) * 8) & ~15u : (v->address + 16) & RAM_MASK;
            irq_due = due + (((address - next) & RAM_MASK) >= 16 ? 7 * SPU_SAMPLE_CYCLES : 0);
            s->fetch_due[n] = due; s->fetch_irq_due[n] = irq_due; s->voice_cached |= 1u << n;
        }
        if (due < s->voice_due) s->voice_due = due;
        if (due < s->voice_refresh) s->voice_refresh = due;
        if (irq_due < s->voice_irq_due) s->voice_irq_due = irq_due;
    }
    s->voice_valid = 1;
}

#if defined(__GNUC__)
__attribute__((noinline))
#endif
static uint64_t batch_deadline(spu_t *s, int irq_only) {
    unsigned samples = 183, control = REG(s, 0x1aa);
    if ((control & 0x8040) == 0x8040 && !(s->status & 0x40)) {
        uint32_t address = (uint32_t)REG(s, 0x1a4) * 8;
        if (s->key_on) return s->next_sample;
        /* Manual FIFO and capture accesses have exact RAM address deadlines. */
        if (((control >> 4) & 3) == 1 && ((address - s->transfer) & RAM_MASK) < s->fifo_count * 2)
            samples = 0;
        if (REG(s, 0x1ac) & 12) {
            const uint16_t *r = &REG(s, 0x1c0);
            unsigned i, delay;
            if (address < 0x1000) {
                delay = ((address - s->capture) & 0x3ff) / 2;
                if (delay < samples) samples = delay;
            }
            for (i = 0; address >= (uint32_t)REG(s, 0x1a2) * 8 && i < 32; ++i) {
                int32_t offset;
                if (i == 10 || i == 11 || i == 18 || i == 19) {
                    if (!(control & 0x80)) continue;
                    delay = reverb_irq_delay(s, (int32_t)r[i] * 8 - 2);
                    if (delay < samples) samples = delay;
                } else if (i >= 26 && i <= 29) {
                    delay = reverb_irq_delay(s, ((int32_t)r[i] - r[(i - 26) / 2]) * 8);
                    if (delay < samples) samples = delay;
                    if (!(control & 0x80)) continue;
                } else if (!((i >= 12 && i <= 17) || (i >= 20 && i <= 25))) continue;
                else if ((i == 16 || i == 17 || i == 24 || i == 25) && !(control & 0x80)) continue;
                offset = (int32_t)r[i] * 8;
                delay = reverb_irq_delay(s, offset);
                if (delay < samples) samples = delay;
            }
        }
        /* Decode boundaries also bound possible ADPCM RAM accesses. Pitch
         * modulation uses the maximum step so it cannot cross an IRQ unseen. */
        if (samples) {
            uint64_t due, limit = s->next_sample + samples * SPU_SAMPLE_CYCLES;
            voice_deadlines(s);
            due = irq_only ? s->voice_irq_due : s->voice_due;
            if (due < limit) return due;
        }
    }
    return s->next_sample + samples * SPU_SAMPLE_CYCLES;
}

uint64_t spu_batch_deadline(spu_t *s) {
    /* MDEC word transfers rebuild the calendar without advancing SPU state.
     * Samples change next_sample; register and DMA access invalidate explicitly. */
    if (!s->batch_valid || s->batch_sample != s->next_sample) {
        s->batch_due = batch_deadline(s, 0);
        s->batch_sample = s->next_sample; s->batch_valid = 1;
    }
    return s->batch_due;
}
uint64_t spu_irq_deadline(spu_t *s) {
    if ((REG(s, 0x1aa) & 0x8040) != 0x8040 || (s->status & 0x40)) return UINT64_MAX;
    if (!s->irq_valid || s->irq_sample != s->next_sample) {
        s->irq_due = batch_deadline(s, 1);
        s->irq_sample = s->next_sample; s->irq_valid = 1;
    }
    return s->irq_due;
}

unsigned spu_dma_irq_words(const spu_t *s) {
    uint32_t address = (uint32_t)REG(s, 0x1a4) * 8;
    if ((REG(s, 0x1aa) & 0x8040) != 0x8040 || (s->status & 0x40)) return UINT32_MAX;
    return ((address - s->transfer) & RAM_MASK) / 4 + 1;
}

/* Skip clocks before the next envelope update, then use the same tick as the
 * full model at the deadline. Re-evaluate after each level/phase transition. */
static unsigned envelope_idle(envelope_t *e, unsigned shift, int exponential,
                              int decrease, int infinite, unsigned count) {
    uint32_t increment = 0x8000u >> (shift > 11 ? shift - 11 : 0);
    unsigned idle;
    if (exponential && !decrease && e->level > 0x6000) {
        if (shift >= 11) increment >>= 2;
        else if (shift == 10) increment >>= 1;
    }
    if (!increment && !infinite) increment = 1;
    if (!increment) return count;
    idle = (0x7fffu - e->counter) / increment;
    if (idle > count) idle = count;
    e->counter += idle * increment;
    return idle;
}

/* Exact fixed-rate clocks. Linear ramps can consume all ticks at once; ADSR
 * phase boundaries cap the span so the following phase starts on its next tick. */
static unsigned envelope_linear(envelope_t *e, unsigned shift, unsigned step_bits,
                                int decrease, int negative, int infinite,
                                unsigned count, int target, int stop) {
    uint32_t increment = 0x8000u >> (shift > 11 ? shift - 11 : 0);
    unsigned first, period, updates, used = count;
    int32_t step = 7 - (int32_t)step_bits, level;
    if (!increment && !infinite) increment = 1;
    if (!increment) return count;
    if (decrease ^ negative) step = ~step;
    step *= 1 << (shift < 11 ? 11 - shift : 0);
    first = (0x7fff - e->counter) / increment + 1;
    if (count < first) { e->counter += count * increment; return count; }
    period = (0x7fff / increment) + 1;
    updates = 1 + (count - first) / period;
    if (stop) {
        unsigned boundary = step > 0 ? (e->level >= target ? 1u : (unsigned)(target - e->level + step - 1) / step) :
            (e->level <= target ? 1u : (unsigned)(e->level - target - step - 1) / (unsigned)-step);
        if (updates >= boundary) { updates = boundary; used = first + (updates - 1) * period; }
    }
    level = e->level + step * (int32_t)updates;
    if (!decrease) e->level = clip(level);
    else if (negative) e->level = level > 0 ? 0 : clip(level);
    else e->level = level < 0 ? 0 : level;
    e->counter = ((used - first) % period) * increment;
    return used;
}

static void adsr_advance(voice_t *v, uint16_t low, uint16_t high, unsigned count) {
    while (count && v->phase) {
        unsigned shift;
        int exponential, decrease, infinite;
        switch (v->phase) {
        case ENV_ATTACK:
            shift = (low >> 10) & 31; exponential = low >> 15; decrease = 0;
            infinite = (low & 0x7f00) == 0x7f00; break;
        case ENV_DECAY:
            shift = (low >> 4) & 15; exponential = decrease = 1; infinite = 0; break;
        case ENV_SUSTAIN:
            shift = (high >> 8) & 31; exponential = high >> 15; decrease = (high >> 14) & 1;
            infinite = (high & 0x1fc0) == 0x1fc0; break;
        default:
            shift = high & 31; exponential = (high >> 5) & 1; decrease = 1; infinite = shift == 31; break;
        }
        if (!exponential) {
            unsigned step = v->phase == ENV_ATTACK ? (low >> 8) & 3 : v->phase == ENV_SUSTAIN ? (high >> 6) & 3 : 0;
            unsigned phase = v->phase;
            int target = phase == ENV_ATTACK ? 32767 : 0;
            uint32_t increment = 0x8000u >> (shift > 11 ? shift - 11 : 0);
            int ticked;
            if (!increment && !infinite) increment = 1;
            ticked = increment && count > (0x7fffu - v->adsr.counter) / increment;
            count -= envelope_linear(&v->adsr, shift, step, decrease, 0, infinite, count, target, phase != ENV_SUSTAIN);
            if (ticked && phase == ENV_ATTACK && v->adsr.level >= 32767) { ++v->phase; v->adsr.counter = 0; }
            else if (ticked && phase == ENV_RELEASE && !v->adsr.level) v->phase = ENV_OFF;
        } else {
            int32_t level = v->adsr.level;
            unsigned phase = v->phase;
            count -= envelope_idle(&v->adsr, shift, exponential, decrease, infinite, count);
            if (count) {
                adsr_tick(v, low, high); --count;
                if (v->phase == phase && v->adsr.level == level) {
                    uint32_t increment = 0x8000u >> (shift > 11 ? shift - 11 : 0);
                    if (!decrease && level > 0x6000) {
                        if (shift >= 11) increment >>= 2;
                        else if (shift == 10) increment >>= 1;
                    }
                    if (!increment && !infinite) increment = 1;
                    if (increment) v->adsr.counter = (count % (0x7fffu / increment + 1)) * increment;
                    count = 0;
                }
            }
        }
    }
}

static void volume_advance(envelope_t *v, uint16_t setting, unsigned count) {
    if (!count) return;
    if (!(setting & 0x8000)) { volume_tick(v, setting); return; }
    if (!(setting & 0x4000)) {
        envelope_linear(v, (setting >> 2) & 31, setting & 3, !!(setting & 0x2000), !!(setting & 0x1000),
                        (setting & 127) == 127, count, 0, 0); return;
    }
    while (count) {
        count -= envelope_idle(v, (setting >> 2) & 31, !!(setting & 0x4000),
                               !!(setting & 0x2000), (setting & 127) == 127, count);
        if (count) { volume_tick(v, setting); --count; }
    }
}

#ifdef FF8_PLAY
/* The caller's independent-voice proof excludes PCM consumers and RAM
 * writes into this source during the span. A full block key and decoder
 * fixed point prove that all subsequent loop decodes have identical output. */
static int voice_decode_loop(spu_t *s, unsigned n, unsigned count, unsigned step) {
    voice_t *v = &s->voices[n]; decode_memo_t *m = &s->decode_memo[n];
    unsigned predictor, total, crossings, final, last_crossed, fetches;
    uint32_t irq;
    if ((!s->replay && !s->control) ||
        (s->irq && (REG(s,0x1aa)&0x8040)==0x8040 && !(s->status&0x40)) ||
        !step || count > 4096 || !v->decoded ||
        (v->flags & 3) != 3 || (uint32_t)REG(s,n*16+14) != v->address/8 ||
        (v->address & 15) || v->address > RAM_MASK-15 || !m->valid ||
        !decode_bytes_equal(m->data, s->ram+v->address) || m->data[1] != v->flags) return 0;
    predictor=m->data[0]>>4;
    if (predictor && predictor<=4 &&
        (m->history[0]!=m->output[27] || m->history[1]!=m->output[26] ||
         v->history[0]!=m->history[0] || v->history[1]!=m->history[1])) return 0;
    total=v->position*4096+v->fraction+step*count;
    crossings=total/(28*4096); final=total%(28*4096);
    last_crossed=crossings && final<step; fetches=crossings-last_crossed;
    adsr_advance(v, REG(s,n*16+8), REG(s,n*16+10), count);
    if(fetches) {
        /* The first new decode retains the old block's interpolation tail;
         * only the second and later ones retain the repeated output tail. */
        if(fetches==1)memcpy(v->samples,v->samples+28,3*sizeof(int16_t));
        else memcpy(v->samples,m->output+25,3*sizeof(int16_t));
        memcpy(v->samples+3,m->output,sizeof(m->output));
        v->history[0]=m->output[27];v->history[1]=m->output[26];
        irq=(uint32_t)REG(s,0x1a4)*8;
        if(irq-v->address<16)ram_irq(s,irq);
    }
    s->blocks+=fetches;
    if(crossings)s->endx|=1u<<n;
    v->decoded=!last_crossed;v->position=final>>12;v->fraction=final&0xfff;
#ifdef FF8_SPU_CACHE_TEST
    s->decode_skipped+=fetches;
#endif
    return 1;
}
#endif

static void voice_advance(spu_t *s, unsigned n, unsigned count) {
    voice_t *v = &s->voices[n];
    uint16_t *r = &REG(s, n * 16);
    unsigned step = r[2] > 0x4000 ? 0x4000 : r[2];
    unsigned delay = v->delay < count ? v->delay : count;
    v->delay -= delay; count -= delay;
    while (count) {
        unsigned span = count, fraction;
        if (!v->decoded) voice_decode(s, n);
#ifdef FF8_PLAY
        if (voice_decode_loop(s,n,count,step)) return;
#endif
        if (step) {
            unsigned boundary = ((28 - v->position) * 4096 - v->fraction + step - 1) / step;
            if (span > boundary) span = boundary;
        }
        adsr_advance(v, r[4], r[5], span);
        fraction = v->fraction + step * span;
        v->position += fraction >> 12; v->fraction = fraction & 0xfff;
        count -= span;
        if (v->position >= 28) {
            v->position -= 28; v->decoded = 0;
            if (v->flags & 1) {
                s->endx |= 1u << n;
                if (v->phase) trace_voice(s, n, "loop-end", (uint32_t)r[7] * 8 & ~15u);
                v->address = ((uint32_t)r[7] * 8) & ~15u;
                if (!(v->flags & 2) && !(flags(s, 0x194) & (1u << n))) {
                    v->phase = ENV_OFF; v->adsr.level = 0;
                }
            } else v->address = (v->address + 16) & RAM_MASK;
        }
    }
}

/* A deferred RAM write can affect both headers and ADPCM history. Prove all
 * fetches in a span disjoint before advancing any voice. Include writes which
 * will occur within this span, and old work areas retained in the journal. */
#ifdef FF8_PLAY
static int voice_source_alias(const spu_t *s, unsigned n, unsigned count, unsigned capture, uint32_t base) {
    const voice_t *v = &s->voices[n];
    unsigned delay = v->delay, decoded = v->decoded, pos = v->position, frac = v->fraction;
    unsigned step = REG(s, n * 16 + 4), left = count, f = v->flags;
    uint32_t addr = v->address, repeat = (uint32_t)REG(s, n * 16 + 14) * 8 & ~15u;
    if (s->key_on & (1u << n)) { delay = 2; decoded = pos = frac = f = 0; addr = (uint32_t)REG(s, n * 16 + 6) * 8 & ~15u; }
    if (n && (flags(s, 0x190) & (1u << n))) step = 0x4000;
    if (step > 0x4000) step = 0x4000;
    if (!(s->key_on & (1u << n)) && decoded && !step) return 0;
    if (decoded && (f & 3) == 3 && repeat == addr && s->ram[(addr + 1) & RAM_MASK] == f) {
        if ((capture && addr < 0x1000) || addr + 16 > base) return 1;
        return 0;
    }
    if (delay >= left) return 0;
    left -= delay;
    while (left) {
        unsigned span = left, fraction;
        if (!decoded) {
            if ((capture && addr < 0x1000) || addr + 16 > base) return 1;
            f = s->ram[(addr + 1) & RAM_MASK]; decoded = 1;
            if (f & 4) repeat = addr;
        }
        if (step) {
            unsigned distance = (28 - pos) * 4096 - frac;
            if (step * span > distance) span = (distance + step - 1) / step;
        }
        fraction = frac + step * span; pos += fraction >> 12; frac = fraction & 0xfff; left -= span;
        if (pos >= 28) { pos -= 28; decoded = 0; addr = f & 1 ? repeat : (addr + 16) & RAM_MASK; }
    }
    return 0;
}
static uint32_t voices_alias_mask(const spu_t *s, unsigned count, uint32_t voices, unsigned capture, uint32_t base) {
    uint32_t aliased = 0;
    unsigned n;
    if (REG(s, 0x1ac) & 12) {
        capture = 1;
        if ((REG(s, 0x1aa) & 0x80) && (uint32_t)REG(s, 0x1a2) * 8 < base)
            base = (uint32_t)REG(s, 0x1a2) * 8;
    }
    if (!capture && base == 0x80000) return 0;
    for (n = 0; n < 24; ++n) if ((voices & (1u << n)) && voice_source_alias(s,n,count,capture,base)) aliased |= 1u << n;
    return aliased;
}
static int voices_alias(const spu_t *s, unsigned count, uint32_t voices, unsigned capture, uint32_t base) {
    unsigned n;
    if (REG(s, 0x1ac) & 12) {
        capture = 1;
        if ((REG(s, 0x1aa) & 0x80) && (uint32_t)REG(s, 0x1a2) * 8 < base)
            base = (uint32_t)REG(s, 0x1a2) * 8;
    }
    if (!capture && base == 0x80000) return 0;
    for (n = 0; n < 24; ++n) if ((voices & (1u << n)) && voice_source_alias(s,n,count,capture,base)) return 1;
    return 0;
}
#else
static int voices_alias(const spu_t *s, unsigned count, uint32_t voices, unsigned capture, uint32_t base) {
    unsigned n;
    if (REG(s, 0x1ac) & 12) {
        capture = 1;
        if ((REG(s, 0x1aa) & 0x80) && (uint32_t)REG(s, 0x1a2) * 8 < base)
            base = (uint32_t)REG(s, 0x1a2) * 8;
    }
    if (!capture && base == 0x80000) return 0;
    for (n = 0; n < 24; ++n) {
        const voice_t *v = &s->voices[n];
        if (!(voices & (1u << n))) continue;
        unsigned delay = v->delay, decoded = v->decoded, pos = v->position, frac = v->fraction;
        unsigned step = REG(s, n * 16 + 4), left = count, f = v->flags;
        uint32_t addr = v->address, repeat = (uint32_t)REG(s, n * 16 + 14) * 8 & ~15u;
        if (s->key_on & (1u << n)) { delay = 2; decoded = pos = frac = f = 0; addr = (uint32_t)REG(s, n * 16 + 6) * 8 & ~15u; }
        if (n && (flags(s, 0x190) & (1u << n))) step = 0x4000;
        if (step > 0x4000) step = 0x4000;
        if (!(s->key_on & (1u << n)) && decoded && !step) continue;
        if (decoded && (f & 3) == 3 && repeat == addr && s->ram[(addr + 1) & RAM_MASK] == f) {
            if ((capture && addr < 0x1000) || addr + 16 > base) return 1;
            continue;
        }
        if (delay >= left) continue;
        left -= delay;
        while (left) {
            unsigned span = left, fraction;
            if (!decoded) {
                if ((capture && addr < 0x1000) || addr + 16 > base) return 1;
                f = s->ram[(addr + 1) & RAM_MASK]; decoded = 1;
                if (f & 4) repeat = addr;
            }
            if (step) {
                unsigned distance = (28 - pos) * 4096 - frac;
                if (step * span > distance) span = (distance + step - 1) / step;
            }
            fraction = frac + step * span; pos += fraction >> 12; frac = fraction & 0xfff; left -= span;
            if (pos >= 28) { pos -= 28; decoded = 0; addr = f & 1 ? repeat : (addr + 16) & RAM_MASK; }
        }
    }
    return 0;
}
#endif


static void spu_advance_legacy(spu_t *s, uint64_t cycles) {
    /* Independent voice spans require immutable RAM and no PCM dependencies.
     * Keep the original sample calendar for DMA4 and IRQ-visible intervals. */
    if (s->backend && s->next_sample <= cycles && !(REG(s, 0x1ac) & 12) &&
        !flags(s, 0x190) && !flags(s, 0x198) && !s->fifo_count &&
        ((REG(s, 0x1aa) & 0x8040) != 0x8040 || (s->status & 0x40))) {
        uint64_t remaining = (cycles - s->next_sample) / SPU_SAMPLE_CYCLES + 1;
        /* Bound integer products even for an unusual direct caller. */
        unsigned count = remaining > 4096 ? 4096 : (unsigned)remaining, n, ch;
        for (n = 0; n < 24; ++n) {
            voice_t *v = &s->voices[n];
            if (s->key_off & (1u << n)) { v->phase = ENV_RELEASE; v->adsr.counter = 0; }
            if (s->key_on & (1u << n)) start_voice(s, n);
            if (!(REG(s, 0x1aa) & 0x8000)) { v->phase = ENV_OFF; v->adsr.level = 0; }
            voice_advance(s, n, count);
            for (ch = 0; ch < 2; ++ch) if (s->volume_work[ch] & (1u << n))
                volume_advance(&v->volume[ch], REG(s, n * 16 + ch * 2),
                               s->volume_sweep[ch] & (1u << n) ? count : 1);
        }
        s->bulk = 0xffffffu; s->bulk_frames += count;
        for (n = 0; n < count; ++n) { sample(s); s->next_sample += SPU_SAMPLE_CYCLES; }
        s->bulk = 0;
    }
    while (s->next_sample <= cycles) { sample(s); s->next_sample += SPU_SAMPLE_CYCLES; }
}

#include "spu_control.inc"

uint16_t spu_read(spu_t *s, unsigned offset) {
    offset &= 0x3fe;
#ifdef FF8_PLAY
    control_pitch_observe(s, offset);
#endif
    if (offset < 0x180 && (offset & 15) == 12) return (uint16_t)s->voices[offset / 16].adsr.level;
    if (offset >= 0x200 && offset < 0x260) return (uint16_t)s->voices[(offset - 0x200) / 4].volume[(offset / 2) & 1].level;
    switch (offset) {
    case 0x19c: return (uint16_t)s->endx;
    case 0x19e: return (uint16_t)(s->endx >> 16);
    case 0x1ae: return s->status | (s->fifo_count && ((REG(s, 0x1aa) >> 4) & 3) == 1 ? 0x400 : 0);
    case 0x1b8: return (uint16_t)s->master[0].level;
    case 0x1ba: return (uint16_t)s->master[1].level;
    default: return REG(s, offset);
    }
}
void spu_write(spu_t *s, unsigned offset, uint16_t value) {
    offset &= 0x3fe;
    if (offset == 0x1aa && ((REG(s,offset) ^ value) & 0x3f00))
        trace_voice(s, 0, "noise-rate", value);
    if (offset < 0x180 && ((offset & 15) == 6 || (offset & 15) == 14))
        trace_voice(s, offset / 16, (offset & 15) == 6 ? "write-start" : "write-repeat", (uint32_t)value * 8);
    if (offset >= 0x188 && offset <= 0x18e) {
        unsigned n, base = (offset & 2) ? 16 : 0;
        for (n = base; n < base + 16 && n < 24; ++n) if (value & (1u << (n - base)))
            trace_voice(s, n, offset < 0x18c ? "KON" : "KOFF", value);
    }
#ifdef FF8_PLAY
    /* An OFF/zero voice cannot expose PCM until KON or writable ENVX revives
     * it. KON resets history; a nonzero ENVX write must restore that history. */
    if (offset < 0x180 && (offset & 15) == 12 && value && s->control &&
        !(s->control->live_pcm_valid & (1u << (offset / 16))))
        control_materialize(s, 6);
#endif
    if ((offset == 0x190 || offset == 0x192) && REG(s, offset) != value) {
#ifdef FF8_PLAY
        if (s->control) {
            s->control->pending_pitch=0; s->control->pitch_live=1;
        } else
#endif
            control_materialize(s, 5);
    }
#ifdef FF8_PLAY
    if (offset == 0x1aa && (value & 0x40) && s->control &&
        s->control->pending_pitch && !control_observe_voices(s))
        control_materialize(s, 5);
#endif
    control_log(s, offset, value, NULL, 0);
    control_uncache(s);
    s->batch_valid = s->irq_valid = s->voice_valid = s->voice_cached = 0;
    if (offset == 0x1ae || offset == 0x19c || offset == 0x19e) return;
    if (s->backend && s->backend->write && (offset < 0x188 || offset > 0x18e))
        s->backend->write(s->backend_context, offset, value);
    REG(s, offset) = value;
    if (offset == 0x1a2 || (offset >= 0x1c0 && offset < 0x200)) s->reverb_dirty = 1;
    if (offset < 0x180) {
        if ((offset & 15) <= 2) {
            unsigned ch = (offset >> 1) & 1;
            uint32_t bit = 1u << (offset / 16);
            s->volume_work[ch] |= bit;
            s->volume_sweep[ch] = (s->volume_sweep[ch] & ~bit) | (value & 0x8000 ? bit : 0);
        }
        if ((offset & 15) == 12) s->voices[offset / 16].adsr.level = (int16_t)value;
        return;
    }
    if (offset >= 0x200 && offset < 0x260) {
        s->volume_work[(offset / 2) & 1] |= 1u << ((offset - 0x200) / 4);
        s->voices[(offset - 0x200) / 4].volume[(offset / 2) & 1].level = (int16_t)value; return;
    }
    switch (offset) {
    case 0x180: case 0x182: {
        unsigned bit = 1u << ((offset >> 1) & 1);
        s->master_work |= bit;
        s->master_sweep = (s->master_sweep & ~bit) | (value & 0x8000 ? bit : 0);
        break;
    }
    case 0x188: s->key_on |= value; break;
    case 0x18a: s->key_on |= (uint32_t)(value & 255) << 16; break;
    case 0x18c: s->key_off |= value; break;
    case 0x18e: s->key_off |= (uint32_t)(value & 255) << 16; break;
    case 0x1a2: s->reverb_address = (uint32_t)value * 8; break;
    case 0x1a6: s->transfer = (uint32_t)value * 8; break;
    case 0x1a8: if (s->fifo_count < 32) s->fifo[s->fifo_count++] = value; break;
    case 0x1aa: if (!(value & 0x40)) s->status &= ~0x40u; break;
    case 0x1b8: s->master[0].level = (int16_t)value; s->master_work |= 1; break;
    case 0x1ba: s->master[1].level = (int16_t)value; s->master_work |= 2; break;
    default: break;
    }
}
uint32_t spu_dma_read(spu_t *s) {
    control_dma_observe(s);
    control_log(s, 0x400, 0, NULL, 0);
    s->batch_valid = s->irq_valid = s->voice_valid = s->voice_cached = 0;
    uint32_t low = spu_ram_read(s, s->transfer), high;
    s->transfer = (s->transfer + 2) & RAM_MASK;
    high = spu_ram_read(s, s->transfer); s->transfer = (s->transfer + 2) & RAM_MASK;
    ++s->dma_words; return low | (high << 16);
}
void spu_dma_write(spu_t *s, uint32_t value) {
    control_log(s, 0x401, value, NULL, 0);
    s->batch_valid = s->irq_valid = s->voice_valid = s->voice_cached = 0;
    spu_ram_write(s, s->transfer, (uint16_t)value);
    if (s->backend && s->backend->upload) s->backend->upload(s->backend_context, s->transfer, s->ram + s->transfer, 2);
    s->transfer = (s->transfer + 2) & RAM_MASK;
    spu_ram_write(s, s->transfer, (uint16_t)(value >> 16));
    if (s->backend && s->backend->upload) s->backend->upload(s->backend_context, s->transfer, s->ram + s->transfer, 2);
    s->transfer = (s->transfer + 2) & RAM_MASK;
    ++s->dma_words;
}
void spu_cd_input(spu_t *s, const int16_t *stereo_44100, unsigned frames) {
    unsigned i;
    if (!s) return;
#ifdef FF8_PLAY
    const unsigned journal_frames = 256;
#else
    const unsigned journal_frames = 16384;
#endif
    if (s->control && frames > journal_frames) {
        while (frames) {
            unsigned part = frames > journal_frames ? journal_frames : frames;
            spu_cd_input(s, stereo_44100, part); stereo_44100 += part * 2; frames -= part;
        }
        return;
    }
    control_log(s, 0x402, frames, stereo_44100, frames * 4);
    s->cd_received += frames;
    for (i = 0; i < frames; ++i) {
        unsigned at;
        if (s->cd_count == CD_FRAMES) { ++s->cd_dropped; continue; }
        at = (s->cd_read + s->cd_count++) % CD_FRAMES;
        s->cd[at][0] = stereo_44100[i * 2]; s->cd[at][1] = stereo_44100[i * 2 + 1];
    }
}
unsigned spu_read_frames(spu_t *s, int16_t *out, unsigned capacity) {
    unsigned n = s->output_count < capacity ? s->output_count : capacity, i;
    for (i = 0; i < n; ++i) {
        unsigned at = (s->output_read + i) % AUDIO_FRAMES;
        out[i * 2] = s->output[at][0]; out[i * 2 + 1] = s->output[at][1];
    }
    s->output_read = (s->output_read + n) % AUDIO_FRAMES; s->output_count -= n; return n;
}
void spu_sync(spu_t *s) { control_materialize(s, 6); }
const uint8_t *spu_ram(const spu_t *s) { control_materialize((spu_t *)s, 6); return s->ram; }

void spu_summary(const spu_t *s, FILE *out) {
    unsigned n;
    spu_control_summary(s, out);
    fprintf(out, "SPU: frames=%llu key-ons=%llu blocks=%llu DMA words=%llu CD frames=%llu dropped=%llu CD dropped=%llu ENDX=%06x\n",
            (unsigned long long)s->frames, (unsigned long long)s->key_ons, (unsigned long long)s->blocks,
            (unsigned long long)s->dma_words, (unsigned long long)s->cd_received,
            (unsigned long long)s->dropped, (unsigned long long)s->cd_dropped, s->endx);
    fprintf(out, "SPU sources: nonzero voice-mix frames=%llu nonzero CD-mix frames=%llu\n",
            (unsigned long long)s->voice_frames, (unsigned long long)s->cd_audible_frames);
    fprintf(out, "SPU work: control=%llu bulk=%llu interpolations=%llu\n",
            (unsigned long long)s->control_frames, (unsigned long long)s->bulk_frames,
            (unsigned long long)s->interpolations);
    fprintf(out, "SPU registers: control=%04x status=%04x IRQ-address=%05x transfer=%05x\n",
            REG(s, 0x1aa), s->status, REG(s, 0x1a4) * 8, s->transfer);
    for (n = 0; n < 24; ++n) if (s->voices[n].phase || n >= 22)
        fprintf(out, "SPU voice %u: address=%05x repeat=%05x flags=%02x phase=%u envelope=%d\n",
                n, s->voices[n].address, REG(s, n * 16 + 14) * 8, s->voices[n].flags,
                s->voices[n].phase, s->voices[n].adsr.level);
}

#if defined(FF8_SAVESTATES)
#define FF8_SS_PRIVATE
#include "savestate_spu.c"
#endif
