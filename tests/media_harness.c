#include "hw.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>

static cpu_t *c;
static int16_t captured[20000 * 2];
static unsigned captured_frames;
void __wrap_spu_cd_input(struct spu *s, const int16_t *pcm, unsigned frames) {
    (void)s;
    assert(captured_frames + frames <= 20000);
    memcpy(captured + captured_frames * 2, pcm, frames * 2 * sizeof(*pcm));
    captured_frames += frames;
}

/* Generate the standard inverse-DCT basis from its cosine equation.
 * The fixture contains no uploaded game table or disc-derived byte array. */
static uint16_t synthetic_scale(unsigned index) {
    unsigned frequency = index / 8, sample = index % 8, k;
    const double pi = 3.14159265358979323846;
    double angle = (2 * sample + 1) * frequency * pi / 16.0;
    double value = 1.0, term = 1.0, scaled;
    int integer;
    while (angle > pi) angle -= 2 * pi;
    /* Range reduction bounds the Taylor remainder well below one Q15 unit. */
    for (k = 1; k <= 16; ++k) {
        term *= -angle * angle / ((2 * k - 1) * (2 * k));
        value += term;
    }
    if (!frequency) value = 0.70710678118654752440;
    scaled = value * 32768.0;
    integer = (int)scaled;
    if ((double)integer > scaled) --integer;
    return (uint16_t)(int16_t)integer;
}
static void advance(unsigned n) { c->cycles += n; hw_event(c); assert(!c->halted); }
static void mw(uint32_t v) { hw_write(c, 0x1f801820, 4, v); }
static uint32_t ms(void) { return hw_read(c, 0x1f801824, 4); }
static uint32_t mr(void) { return hw_read(c, 0x1f801820, 4); }
static void tables(void) {
    unsigned i;
    hw_write(c, 0x1f801824, 4, 0xe0000000);
    assert(ms() == 0x80040000);
    mw(0x40000001);
    assert(ms() & 0x10000000);
    for (i = 0; i < 32; ++i) mw(0x10101010);
    mw(0x60000000);
    for (i = 0; i < 32; ++i) mw(synthetic_scale(i * 2) | (uint32_t)synthetic_scale(i * 2 + 1) << 16);
    assert((ms() & 0x2000ffff) == 0xffff);
}
static void start_dma(unsigned ch, uint32_t address, unsigned words) {
    hw_write(c, 0x1f801080 + ch * 16, 4, address);
    hw_write(c, 0x1f801084 + ch * 16, 4, 0x10000 | words);
    hw_write(c, 0x1f801088 + ch * 16, 4, 0x1000200 | (ch == 0));
}

static void mdec_tests(void) {
    unsigned i, d;
    tables();
    for (d = 0; d < 4; ++d) {
        unsigned bytes = d == 0 ? 32 : d == 1 ? 64 : d == 2 ? 768 : 512;
        uint32_t expected = d == 0 ? 0x88888888 : d == 3 ? 0x42104210 : 0x80808080;
        mw(0x20000000 | (d << 27) | (d < 2 ? 1 : 6));
        for (i = 0; i < (d < 2 ? 1u : 6u); ++i) mw(0xfe000400);
        assert(ms() & 0x80000000u);
        advance(d < 2 ? 511 : 1535); assert(ms() & 0x80000000u);
        advance(1); assert(!(ms() & 0x80000000u));
        assert(!!(ms() & 0x08000000) == (d >= 2));
        for (i = 0; i < bytes / 4; ++i) {
            if (ms() & 0x80000000u) advance(512);
            assert(mr() == expected);
        }
        assert(ms() & 0x80000000u);
    }
    /* Signed monochrome and 15-bit output mask. */
    mw(0x2c000001); mw(0xfe000400); advance(512);
    for (i = 0; i < 16; ++i) assert(mr() == 0);
    mw(0x3a000006);
    for (i = 0; i < 6; ++i) mw(0xfe000400);
    advance(1024);
    for (i = 0; i < 128; ++i) { advance(512); assert(mr() == 0xc210c210); }
    /* Positive signed saturation must round before XORing the five-bit bias. */
    mw(0x3c000006);
    for (i = 0; i < 6; ++i) mw(0xfe000400 | (i < 2 ? 0 : 100));
    advance(1024);
    for (i = 0; i < 128; ++i) { advance(512); assert(mr() == 0x3def3def); }
    /* DC=8 and first horizontal AC=8, qscale=1, quant=16. */
    mw(0x28000002); mw(0x00080408); mw(0xfe00fe00); advance(512);
    for (i = 0; i < 8; ++i) { assert(mr() == 0x90919293); assert(mr() == 0x8d8e8e8f); }
    /* qscale=0 bypasses quantization and zigzag: second coefficient at k=2. */
    mw(0x28000002); mw(0x04080008); mw(0xfe00fe00); advance(512);
    for (i = 0; i < 8; ++i) { assert(mr() == 0x7f818385); assert(mr() == 0x8583817f); }
    /* Partial coefficients survive separate writes; reset discards queued data. */
    mw(0x28000002); mw(0x04010408); assert(ms() & 0x20000000);
    hw_write(c, 0x1f801824, 4, 0x80000000); assert(ms() == 0x80040000);
    mw(0x00001234); assert((ms() & 0xffff) == 0x1234);
    mw(0x28000100); mw(0xfe000400);
    for (i = 0; i < 32; ++i) mw(0xfe00fe00);
    assert(ms() & 0x40000000);
    assert(!(ms() & 0x10000000));
    hw_write(c, 0x1f801824, 4, 0x80000000); assert(ms() == 0x80040000);
}

static void mdec_dma_tests(void) {
    unsigned i;
    tables();
    hw_write(c, 0x1f8010f0, 4, 0x88);
    hw_write(c, 0x1f8010f4, 4, 0x830000);
    start_dma(1, 0x10000, 128);
    advance(1000);
    assert(c->hw->dma[1].chcr & 0x1000000);
    assert(c->hw->dma[1].completions == 0);
    mw(0x38000006);
    /* Cr=Cb=0; four luma blocks with DC 0,8,16,24 and quant[0]=16.
     * Their constant intensities are 128,144,160,176. */
    for (i = 0; i < 6; ++i) mem_write32(c, 0x2000 + i * 4, 0xfe000400 | (i < 2 ? 0 : (i - 2) * 8));
    start_dma(0, 0x2000, 6);
    advance(5000);
    assert(c->hw->dma[0].completions == 1 && c->hw->dma[1].completions == 1);
    assert((c->hw->dicr & 0x83000000u) == 0x83000000u);
    for (i = 0; i < 256; ++i) {
        unsigned b = (i % 16 >= 8) + (i / 16 >= 8) * 2, v = 16 + b * 2;
        assert(mem_read16(c, 0x10000 + i * 2) == (v | (v << 5) | (v << 10)));
    }
    mw(0x30000006);
    start_dma(1, 0x10000, 192);
    start_dma(0, 0x2000, 6);
    advance(5000);
    for (i = 0; i < 256 * 3; ++i) {
        unsigned pixel = i / 3, b = (pixel % 16 >= 8) + (pixel / 16 >= 8) * 2;
        assert(mem_read8(c, 0x10000 + i) == 128 + b * 16);
    }
    /* Stream more than the input FIFO, holding DMA1 off until backpressure. */
    mw(0x38000258);
    for (i = 0; i < 600; ++i) mem_write32(c, 0x2000 + i * 4, 0xfe000400);
    start_dma(0, 0x2000, 600); advance(4000);
    assert(c->hw->dma[0].chcr & 0x1000000);
    start_dma(1, 0x10000, 12800); advance(400000);
    assert(!(c->hw->dma[0].chcr & 0x1000000) && !(c->hw->dma[1].chcr & 0x1000000));
    assert(c->hw->dma[0].completions == 3 && c->hw->dma[1].completions == 3);
}

static void movie_dma_overlap(unsigned mode) {
    unsigned i;
    const unsigned output_words = 14 * 192;
    tables();
    /* FF8 uploads a 16x224 RGB24 column, then decodes the next column
     * into the same RAM buffer. Equal priorities favour DMA2 over DMA1. */
    hw_write(c, 0x1f8010f0, 4, mode == 1 ? 0xb8b : 0xbbb);
    for (i = 0; i < output_words; ++i)
        mem_write32(c, 0x10000 + i * 4, 0x11111111u + i);
    gpu_gp0(c->hw->gpu, 0xa0000000);
    gpu_gp0(c->hw->gpu, 0);
    gpu_gp0(c->hw->gpu, (224u << 16) | 24);
    hw_write(c, 0x1f8010a0, 4, 0x10000);
    hw_write(c, 0x1f8010a4, 4, (84u << 16) | 32);
    if (mode != 3) hw_write(c, 0x1f8010a8, 4, 0x01000201);
    mw(0x30000054);
    for (i = 0; i < 84; ++i) mem_write32(c, 0x2000 + i * 4, 0xfe000400);
    start_dma(0, 0x2000, 84);
    start_dma(1, 0x10000, output_words);
    if (mode == 3) {
        for (i = 0; c->hw->dma[1].due == HW_NEVER && i < 2000; ++i) advance(1);
        assert(i < 2000);
        hw_write(c, 0x1f8010a8, 4, 0x01000201);
    }
    advance(2000);
    assert(c->hw->dma[2].chcr & 0x1000000);
    assert(c->hw->dma[1].chcr & 0x1000000);
    assert(mem_read32(c, 0x10000) == (mode == 1 ? 0x80808080u : 0x11111111u));
    if (mode == 2) {
        hw_write(c, 0x1f8010a8, 4, 0);
        advance(2);
        assert(mem_read32(c, 0x10000) == 0x80808080);
        advance(60000);
        assert(c->hw->dma[1].completions == 1 && c->hw->dma[2].completions == 0);
        return;
    }
    advance(58000);
    assert(c->hw->dma[1].completions == 1 && c->hw->dma[2].completions == 1);
    if (mode == 1) return;
    for (i = 0; i < output_words * 2; ++i) {
        uint32_t word = 0x11111111u + i / 2;
        assert(gpu_vram(c->hw->gpu)[i / 24 * 1024 + i % 24] ==
               (uint16_t)(word >> ((i & 1) * 16)));
    }
    for (i = 0; i < output_words; ++i) assert(mem_read32(c, 0x10000 + i * 4) == 0x80808080);
}

static void cw(unsigned port, unsigned v) { hw_write(c, 0x1f801800 + port, 1, v); }
static void ack(void) { cw(0, 1); cw(3, 0x1f); }
static void cmd(unsigned command, const uint8_t *p, unsigned n) {
    unsigned i;
    cw(0, 0); for (i = 0; i < n; ++i) cw(2, p[i]); cw(1, command);
    advance(25000); assert(c->hw->cd.irq_flags == 3); ack();
}
static void cd_stream(const char *path, unsigned mode, int mix) {
    uint8_t m = (uint8_t)mode, filter[2] = {1,2};
    char error[256];
    assert(cdrom_open(c, path, error, sizeof(error)));
    cmd(0xd, filter, 2); cmd(0xe, &m, 1);
    if (mix) {
        cw(0, 2); cw(2, 0); cw(3, 0x80);
        cw(0, 3); cw(1, 0); cw(2, 0x80);
        if (mix != 2) cw(3, mix == 3 ? 0x21 : 0x20);
    }
    cmd(0x1b, NULL, 0);
    if (!(mode & 0x48)) {
        unsigned i;
        for (i = 0; i < 4; ++i) {
            advance(HW_CPU_HZ / 150); assert(c->hw->cd.irq_flags == 1);
            cw(0, 0); cw(3, 0); cw(3, 0x80);
            assert(c->hw->cd.data_lba == i);
            assert(cdrom_dma(c) == ((i == 0 || i == 3) ? 0x11223344u : 0));
            ack();
        }
        assert(!captured_frames && !c->hw->cd.xa_sectors);
        return;
    }
    advance(HW_CPU_HZ / 150); assert(c->hw->cd.irq_flags == 1); ack();
    advance(HW_CPU_HZ / 150); assert(c->hw->cd.irq_flags == 0);
    /* The audio sector must not replace the previous data sector's bytes. */
    cw(0, 0); cw(3, 0x80);
    assert(c->hw->cd.data_lba == 0 && cdrom_dma(c) == 0x11223344);
    advance(HW_CPU_HZ / 150); assert(c->hw->cd.irq_flags == 0);
    advance(HW_CPU_HZ / 150); assert(c->hw->cd.irq_flags == 1);
    assert(c->hw->cd.buffer_lba == 3);
    assert(c->hw->cd.xa_sectors == ((mode & 0x40) ? 1u : 0u));
    assert(c->hw->cd.xa_filtered == ((mode & 0x40) ? 1u : 2u));
    assert(captured_frames == c->hw->cd.xa_frames);
    assert(fwrite(captured, sizeof(int16_t) * 2, captured_frames, stdout) == captured_frames);
}

int main(int argc, char **argv) {
    assert(argc >= 2);
    c = calloc(1, sizeof(*c)); assert(c); cpu_init(c); hw_get(c);
    if (!strcmp(argv[1], "mdec")) mdec_tests();
    else if (!strcmp(argv[1], "dma")) mdec_dma_tests();
    else if (!strcmp(argv[1], "movie-overlap")) movie_dma_overlap(0);
    else if (!strcmp(argv[1], "movie-priority")) movie_dma_overlap(1);
    else if (!strcmp(argv[1], "movie-cancel")) movie_dma_overlap(2);
    else if (!strcmp(argv[1], "movie-late-gpu")) movie_dma_overlap(3);
    else if (!strcmp(argv[1], "idct")) {
        int16_t input[64], matrix[64];
        assert(fread(input, 2, 64, stdin) == 64 && fread(matrix, 2, 64, stdin) == 64);
        mdec_idct(input, matrix); assert(fwrite(input, 2, 64, stdout) == 64);
    } else if (!strcmp(argv[1], "xa")) {
        uint8_t sector[2352]; int16_t pcm[4032 * 2]; unsigned n;
        assert(fread(sector, 1, 2352, stdin) == 2352);
        n = cdrom_decode_xa(&c->hw->cd, sector, pcm);
        assert(fwrite(pcm, 4, n, stdout) == n);
        n = cdrom_decode_xa(&c->hw->cd, sector, pcm);
        assert(fwrite(pcm, 4, n, stdout) == n);
    } else if (!strcmp(argv[1], "stream")) {
        assert(argc == 5); cd_stream(argv[2], (unsigned)strtoul(argv[3], NULL, 0), atoi(argv[4]));
    } else abort();
    hw_destroy(c); free(c); return 0;
}
