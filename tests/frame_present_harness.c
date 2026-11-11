/* The live presenter (runtime/frame_present.h) without a game:
 *   frame_present phases
 *     prints the newest frame's fraction at each VBlank for game frames
 *     that end every 2, 3, 4 or 6 VBlanks, a change of rate, a gap and a
 *     frame that must be shown whole;
 *   frame_present waiting
 *     draws a guest that fills its screen at every VBlank and ends no game
 *     frame, as before a game's first, and prints the words drawn again for
 *     the first picture and the last, and whether the last is the guest's;
 *   frame_present replay RECORDING OUT.rgb [fractions]
 *     plays a recording (runtime/frame_record.h) into a GPU as the guest's,
 *     with the presenter tapping it as the player does, and writes the picture
 *     it presents at each VBlank, centred on a 320x240 canvas as
 *     tools/devtools/inbetween.c writes its own: the two must agree. */
#include "../runtime/gpu.c"
#include "frame_present.h"
#include "frame_record.h"
#include <inttypes.h>
#include <stdio.h>
#include <string.h>

static void phases(const char *name, const uint64_t *ends, unsigned count, const int *whole, uint64_t until) {
    frame_present_clock_t c;
    uint64_t v;
    unsigned next = 0;
    frame_present_clock_reset(&c);
    printf("%s:", name);
    for (v = ends[0]; v <= until; ++v) {
        uint32_t num, den;
        int between;
        /* The picture for VBlank v comes at the VBlank; a frame that ends
         * at v ends later in its period, as the player sees them. */
        between = frame_present_clock_phase(&c, (double)v, &num, &den);
        if (v > ends[0]) {
            if (between) printf(" %" PRIu64 "=%.2f", v, (double)num / den);
            else printf(" %" PRIu64 "=whole", v);
        }
        if (next < count && ends[next] == v) {
            frame_present_clock_frame(&c, v, !(whole && whole[next]));
            ++next;
        }
    }
    printf(" between=%" PRIu64 " holds=%" PRIu64 " skips=%" PRIu64 "\n", c.between, c.holds, c.skips);
}

static int run_phases(void) {
    static const uint64_t two[] = {10, 12, 14, 16}, three[] = {10, 13, 16, 19}, four[] = {10, 14, 18, 22},
                          six[] = {10, 16, 22, 28}, faster[] = {10, 14, 18, 20, 22}, slower[] = {10, 12, 14, 18, 22},
                          gap[] = {10, 12, 14, 30, 32}, shown[] = {10, 14, 18, 22};
    static const int whole[] = {0, 0, 1, 0};
    uint32_t num, den;
    frame_present_clock_t c;
    phases("two", two, 4, NULL, 18);
    phases("three", three, 4, NULL, 22);
    phases("four", four, 4, NULL, 26);
    phases("six", six, 4, NULL, 34);
    phases("faster", faster, 5, NULL, 24);
    phases("slower", slower, 5, NULL, 26);
    phases("gap", gap, 5, NULL, 34);
    phases("whole", shown, 4, whole, 26);
    /* Between VBlanks the fraction follows the moment: a quarter of the way
     * through a four-VBlank interval one VBlank in, three eighths half a
     * VBlank later. */
    frame_present_clock_reset(&c);
    frame_present_clock_frame(&c, 10, 1);
    frame_present_clock_frame(&c, 14, 1);
    frame_present_clock_phase(&c, 15, &num, &den);
    printf("moment: 15=%u/%u", num, den);
    frame_present_clock_phase(&c, 15.5, &num, &den);
    printf(" 15.5=%u/%u\n", num, den);
    return 0;
}

static uint32_t le32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static uint64_t le64(const uint8_t *p) { return le32(p) | (uint64_t)le32(p + 4) << 32; }

static frame_present_t *present;
static uint64_t polygons;
static void tap_gp0(void *context, const uint8_t *words, unsigned count) { (void)context; frame_present_gp0(present, words, count); }
static void tap_gp1(void *context, uint32_t word) { (void)context; frame_present_gp1(present, word); }
static void tap_read(void *context) { (void)context; frame_present_read(present); }
static void tap_vblank(void *context) { (void)context; frame_present_vblank(present); }
static const gpu_tap_t tap = {tap_gp0, tap_gp1, tap_read, tap_vblank};
static void ignore_start(void *context, uint32_t start) { (void)context; (void)start; }
static void ignore_vblank(void *context) { (void)context; }
static void count_polygon(void *context, unsigned opcode, const uint32_t *packet) { (void)context; (void)opcode; (void)packet; ++polygons; }
/* The identity service reports each command before it counts its polygon. */
static void command(void *context, const uint32_t *packet, unsigned count) {
    (void)context;
    frame_present_command(present, packet, count, polygons);
}
static const gpu_observer_t observer = {ignore_start, ignore_vblank, count_polygon, command};

static void expand(uint16_t v, uint8_t *o) {
    unsigned i;
    for (i = 0; i < 3; ++i) { unsigned x = (v >> (i * 5)) & 31; o[i] = (uint8_t)((x << 3) | (x >> 2)); }
}
static void picture(gpu_t *g, uint8_t *out) {
    int x, y, w, h, rgb24, enabled, i, j;
    memset(out, 0, 320 * 240 * 3);
    gpu_display(g, &x, &y, &w, &h, &rgb24, &enabled);
    if (!enabled || rgb24) return;
    for (j = 0; j < h; ++j) {
        int oy = j + (240 - h) / 2;
        if (oy < 0 || oy >= 240) continue;
        for (i = 0; i < w; ++i) {
            int ox = i + (320 - w) / 2;
            if (ox < 0 || ox >= 320) continue;
            expand(gpu_vram(g)[((unsigned)(y + j) & 511) * 1024 + ((unsigned)(x + i) & 1023)], out + ((size_t)oy * 320 + ox) * 3);
        }
    }
}

static int run_replay(const char *path, const char *out_path, int fractions) {
    static uint8_t rgb[320 * 240 * 3];
    frame_record_reader_t in;
    frame_record_frame_t frame;
    gpu_t *guest = gpu_create();
    FILE *out = fopen(out_path, "wb");
    uint64_t pictures = 0;
    int synced = 0;
    if (!guest || !out || !(present = frame_present_create()) || !frame_record_open(&in, path)) return 2;
    memset(&frame, 0, sizeof(frame));
    frame_present_fractions(present, fractions);
    gpu_observe(guest, &observer, NULL);
    while (frame_record_next(&in)) {
        const uint8_t *p = in.payload;
        if (in.tag == FRAME_RECORD_SNAP) {
            ss_io io;
            memset(&io, 0, sizeof(io));
            io.data = (uint8_t *)p + 24; io.size = in.size - 24; io.reading = 1; io.version = 1;
            ss_gpu(&io, guest);
            if (io.failed) return 2;
            polygons = le64(p + 8);
            gpu_tap(guest, &tap, NULL);
            frame_present_sync(present, guest, polygons);
            synced = 1;
        } else if (!synced) continue;
        else if (in.tag == FRAME_RECORD_GP0) gpu_gp0_words(guest, p, (unsigned)(in.size / 4));
        else if (in.tag == FRAME_RECORD_GP1) gpu_gp1(guest, le32(p));
        else if (in.tag == FRAME_RECORD_READ) (void)gpu_read(guest);
        else if (in.tag == FRAME_RECORD_FRAM) {
            int x, y, w, h, rgb24, enabled;
            if (!frame_record_parse(&frame, p, in.size)) return 2;
            gpu_display(guest, &x, &y, &w, &h, &rgb24, &enabled);
            if (!frame_present_frame(present, &frame.frame, w, h)) return 2;
        } else if (in.tag == FRAME_RECORD_VBLK) {
            /* As the player: the VBlank, then the picture for it. */
            gpu_vblank(guest);
            frame_present_draw(present, (double)frame_present_vblanks(present));
            picture(frame_present_gpu(present), rgb);
            if (fwrite(rgb, 1, sizeof(rgb), out) != sizeof(rgb)) return 2;
            ++pictures;
        } else if (in.tag == FRAME_RECORD_END) break;
    }
    {
        frame_present_totals_t t = frame_present_totals(present);
        const frame_present_clock_t *k = frame_present_timing(present);
        printf("pictures=%" PRIu64 " blended=%" PRIu64 " between=%" PRIu64 " holds=%" PRIu64 " skips=%" PRIu64
               " advances=%" PRIu64 " failures=%" PRIu64 "\n", pictures, t.blended, k->between, k->holds, k->skips,
               t.advances, t.failures);
    }
    frame_record_close(&in);
    frame_record_frame_free(&frame);
    frame_present_destroy(present);
    free(guest);
    return fclose(out) ? 2 : 0;
}

static int run_waiting(void) {
    enum { VBLANKS = 200 };
    gpu_t *guest = gpu_create();
    uint64_t first = 0, last = 0;
    unsigned v;
    if (!guest || !(present = frame_present_create())) return 2;
    gpu_tap(guest, &tap, NULL);
    frame_present_sync(present, guest, 0);
    for (v = 0; v < VBLANKS; ++v) {
        /* A fill of a different colour each time: 64 by 64 at (16, 16). */
        uint32_t fill[3];
        uint8_t bytes[12];
        uint64_t before = frame_present_totals(present).replayed_words;
        unsigned i;
        fill[0] = 0x02000000u | (v + 1); fill[1] = 16u << 16 | 16u; fill[2] = 64u << 16 | 64u;
        for (i = 0; i < 12; ++i) bytes[i] = (uint8_t)(fill[i / 4] >> (i % 4 * 8));
        gpu_gp0_words(guest, bytes, 3);
        gpu_vblank(guest);
        frame_present_draw(present, (double)frame_present_vblanks(present));
        last = frame_present_totals(present).replayed_words - before;
        if (!v) first = last;
    }
    printf("waiting: first=%" PRIu64 " last=%" PRIu64 " same=%d failures=%" PRIu64 "\n", first, last,
           !memcmp(gpu_vram(frame_present_gpu(present)), gpu_vram(guest), 1024 * 512 * sizeof(uint16_t)),
           frame_present_totals(present).failures);
    frame_present_destroy(present);
    free(guest);
    return 0;
}

int main(int argc, char **argv) {
    if (argc > 1 && !strcmp(argv[1], "phases")) return run_phases();
    if (argc > 1 && !strcmp(argv[1], "waiting")) return run_waiting();
    if (argc > 3 && !strcmp(argv[1], "replay")) return run_replay(argv[2], argv[3], argc > 4 && !strcmp(argv[4], "fractions"));
    fprintf(stderr, "usage: frame_present phases | waiting | replay RECORDING OUT.rgb [fractions]\n");
    return 2;
}
