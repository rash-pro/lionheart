#include "cpu.h"
#include "hw.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint32_t status_at(cpu_t *c, unsigned line, int pal) {
    uint64_t numerator = (uint64_t)line * HW_CPU_HZ * (pal ? 1 : 1001);
    unsigned denominator = pal ? 50 * 314 : 60000 * 263;
    c->cycles = (numerator + denominator - 1) / denominator;
    return hw_read(c, 0x1f801814, 4);
}

static void expect(cpu_t *c, unsigned line, int pal, unsigned odd) {
    uint32_t value = status_at(c, line, pal);
    if ((value >> 31) != odd) {
        fprintf(stderr, "GPUSTAT line=%u pal=%d value=%08x expected_odd=%u\n", line, pal, value, odd);
        abort();
    }
}

int main(int argc, char **argv) {
    cpu_t *c = malloc(sizeof(*c));
    int pal;
    assert(argc == 2 && c);
    cpu_init(c);
    pal = !strcmp(argv[1], "pal");
    hw_write(c, 0x1f801814, 4, 0x08000001 | (pal ? 8 : 0));
    hw_write(c, 0x1f801814, 4, 0x07000010 | ((pal ? 288u : 240u) << 10));
    if (!strcmp(argv[1], "transition")) {
        uint32_t prior;
        hw_write(c, 0x1f801814, 4, 0x08000027);
        prior = status_at(c, 16, 0);
        assert(prior & (1u << 22));
        hw_write(c, 0x1f801814, 4, 0x08000001);
        /* VSync can retain its old interlace decision across a mode change.
         * A 240-line scanline transition must still release its bit-31 wait. */
        assert((status_at(c, 17, 0) ^ prior) & (1u << 31));
    } else if (!strcmp(argv[1], "480i")) {
        hw_write(c, 0x1f801814, 4, 0x08000027);
        expect(c, 17, 0, 0); expect(c, 18, 0, 0);
        /* Preserve existing 480i field timing, including its VBlank phase. */
        expect(c, 241, 0, 1);
        /* A field lasts a frame; the next field's visible rows agree. */
        expect(c, 263 + 16, 0, 1); expect(c, 263 + 17, 0, 1);
        expect(c, 263 + 240, 0, 0);
    } else {
        expect(c, 15, pal, 0);
        expect(c, 16, pal, 0); expect(c, 17, pal, 1); expect(c, 18, pal, 0);
        expect(c, pal ? 287 : 239, pal, 1);
        expect(c, pal ? 288 : 240, pal, 0);
    }
    hw_destroy(c); free(c);
    return 0;
}
