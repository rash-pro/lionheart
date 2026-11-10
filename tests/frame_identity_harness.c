/* Drives the frame identity service with synthetic projections and GPU
 * packets, no game data, and prints every frame it delivers:
 *   F<number> flags=<R reset, E excluded, O overflow or -> runs=<n> projections=<n> polygons=<n> vblank=<n>
 *   R<i> first=<n> count=<n> previous=<run or -> repeats=<n>
 *   P<i> run=<n> previous=<projection or -> match=<projection or ->
 *   Q<i> <corner>... with L<age>:<projection> linked, X<age>:<projection> excluded, or
 *        N no projection, S split, U unpaired, A ambiguous, H no history
 * Usage: frame_identity_harness SCENARIO */
#include "frame_identity.h"
#include "gpu.h"
#include "gte.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static cpu_t *c;
static gpu_t *g;
static uint32_t start = 0x100, exclude;
static int failures;

static void check(int ok, const char *what) {
    if (!ok) { printf("FAIL %s\n", what); ++failures; }
}

static void print_index(int32_t i) {
    if (i < 0) printf("-"); else printf("%d", (int)i);
}

static void sink(void *context, const frame_identity_frame_t *f) {
    uint32_t i, k;
    (void)context;
    printf("F%llu flags=%s%s%s%s runs=%u projections=%u polygons=%u vblank=%llu\n", (unsigned long long)f->number,
           f->flags & FRAME_IDENTITY_RESET ? "R" : "", f->flags & FRAME_IDENTITY_EXCLUDED ? "E" : "",
           f->flags & FRAME_IDENTITY_OVERFLOW ? "O" : "", f->flags ? "" : "-",
           f->run_count, f->projection_count, f->polygon_count, (unsigned long long)f->vblank);
    for (i = 0; i < f->run_count; ++i) {
        printf("R%u first=%u count=%u previous=", i, f->runs[i].first, f->runs[i].count);
        print_index(f->runs[i].previous);
        printf(" repeats=%u\n", f->runs[i].repeats);
    }
    for (i = 0; i < f->projection_count; ++i) {
        printf("P%u run=%u previous=", i, f->projections[i].run);
        print_index(f->projections[i].previous);
        printf(" match=");
        print_index(f->projections[i].match);
        printf("\n");
    }
    for (i = 0; i < f->polygon_count; ++i) {
        const frame_identity_polygon_t *q = &f->polygons[i];
        printf("Q%u", i);
        for (k = 0; k < q->corners; ++k) {
            static const char held[] = "LNSUAHX";
            char s = held[q->state[k]];
            if (s == 'L' || s == 'X') printf(" %c%u:%d", s, q->age[k], (int)q->projection[k]);
            else printf(" %c", s);
        }
        printf("\n");
    }
}

static uint32_t classify(void *context, const cpu_t *cpu) {
    (void)context; (void)cpu;
    return exclude;
}

/* A camera: identity rotation (1.0 is 4096), translation, H and screen centre. */
static void camera(int tx, int ty, int tz) {
    unsigned i;
    for (i = 0; i < 5; ++i) c->gte_ctrl[i] = 0;
    c->gte_ctrl[0] = 4096; c->gte_ctrl[2] = 4096; c->gte_ctrl[4] = 4096;
    c->gte_ctrl[5] = (uint32_t)tx; c->gte_ctrl[6] = (uint32_t)ty; c->gte_ctrl[7] = (uint32_t)tz;
    c->gte_ctrl[24] = 160u << 16; c->gte_ctrl[25] = 120u << 16; c->gte_ctrl[26] = 256;
}

static void vertex(unsigned v, int x, int y, int z) {
    c->gte_data[v * 2] = (uint32_t)(uint16_t)x | (uint32_t)(uint16_t)y << 16;
    c->gte_data[v * 2 + 1] = (uint32_t)(uint16_t)z;
}

/* One vertex through RTPS; returns its SXY. */
static uint32_t rtps(int x, int y, int z) {
    vertex(0, x, y, z);
    frame_identity_gte_command(c, (1u << 19) | 0x01);
    return c->gte_data[14];
}

/* Three through RTPT; the results land in out. */
static void rtpt(const int v[9], uint32_t out[3]) {
    unsigned i;
    for (i = 0; i < 3; ++i) vertex(i, v[i * 3], v[i * 3 + 1], v[i * 3 + 2]);
    frame_identity_rtpt(c, 12, 0);
    for (i = 0; i < 3; ++i) out[i] = c->gte_data[12 + i];
}

static void swap(void) { start ^= 0x40000; gpu_gp1(g, 0x05000000u | start); }
static void vblanks(unsigned n) { while (n--) gpu_vblank(g); }

/* Flat triangle (0x20), textured gouraud quad (0x3c) or gouraud triangle (0x30). */
static void triangle(uint32_t a, uint32_t b, uint32_t d) {
    gpu_gp0(g, 0x20808080u); gpu_gp0(g, a); gpu_gp0(g, b); gpu_gp0(g, d);
}
static void quad(const uint32_t xy[4]) {
    unsigned i;
    for (i = 0; i < 4; ++i) {
        gpu_gp0(g, i ? 0x00808080u : 0x3c808080u);
        gpu_gp0(g, xy[i]);
        gpu_gp0(g, (i == 0 ? 0x00000000u : i == 1 ? 0x00000000u : 0) | (i * 8u));
    }
}
static void gouraud(uint32_t a, uint32_t b, uint32_t d) {
    gpu_gp0(g, 0x30101010u); gpu_gp0(g, a); gpu_gp0(g, 0x00202020u); gpu_gp0(g, b);
    gpu_gp0(g, 0x00303030u); gpu_gp0(g, d);
}

static void start_service(unsigned repeat_pixels) {
    frame_identity_config_t config;
    memset(&config, 0, sizeof(config));
    config.sink = sink; config.classify = classify; config.repeat_pixels = repeat_pixels;
    frame_identity_start(g, &config);
    gpu_gp1(g, 0x05000000u | start);
}

/* A model of two parts: a triangle under one setup, two points under another. */
static void model(int dx, int dz, uint32_t out[5]) {
    static const int body[9] = {0, 0, 0, 100, 0, 0, 0, 100, 0};
    camera(dx, 0, 1000 + dz);
    rtpt(body, out);
    camera(dx + 300, 0, 1000 + dz);
    out[3] = rtps(0, 0, 0);
    out[4] = rtps(50, 50, 0);
}

static void scenario_runs(void) {
    uint32_t a[5], b[5];
    start_service(0);
    model(0, 0, a);
    camera(0, 0, 1000);
    rtps(7, 7, 7);                 /* the first setup again: a run of its own */
    swap(); vblanks(2);
    model(10, 5, b);
    camera(10, 0, 1005);
    rtps(7, 7, 8);                 /* changed input: held */
    swap(); vblanks(2);
    model(20, 10, b);
    rtps(1, 2, 3);                 /* the last run grows by one: a new key, held */
    swap();
}

static void scenario_repeats(unsigned guard) {
    static const int dx[3] = {0, 200, 400};
    unsigned i, f;
    start_service(guard);
    /* Three identical instances; they move 4 units a frame. */
    for (f = 0; f < 2; ++f) {
        for (i = 0; i < 3; ++i) { camera(dx[i] + (int)f * 4, 0, 1000); rtps(0, 0, 0); rtps(10, 0, 0); }
        swap(); vblanks(2);
    }
    /* Two of them swap places within a frame: still nearest to themselves. */
    for (i = 0; i < 3; ++i) { camera(dx[2 - i] + 8, 0, 1000); rtps(0, 0, 0); rtps(10, 0, 0); }
    swap(); vblanks(2);
    /* Nearly equidistant: the margin holds both. */
    camera(100, 0, 1000); rtps(0, 0, 0); rtps(10, 0, 0);
    camera(310, 0, 1000); rtps(0, 0, 0); rtps(10, 0, 0);
    swap(); vblanks(2);
    /* One instance moves far on screen: clearly nearest, but beyond the guard. */
    camera(0, 0, 1000); rtps(0, 0, 0); rtps(10, 0, 0);
    camera(1000, 0, 1000); rtps(0, 0, 0); rtps(10, 0, 0);
    swap(); vblanks(2);
    camera(4, 0, 1000); rtps(0, 0, 0); rtps(10, 0, 0);
    camera(1100, 0, 1000); rtps(0, 0, 0); rtps(10, 0, 0);
    swap();
}

static void scenario_gaps(void) {
    start_service(0);
    camera(0, 0, 1000); rtps(1, 1, 1); swap(); vblanks(8);
    camera(0, 0, 1000); rtps(1, 1, 1); swap(); vblanks(9);
    camera(0, 0, 1000); rtps(1, 1, 1); swap(); vblanks(1);
    /* The same start again is not a new frame. */
    gpu_gp1(g, 0x05000000u | start);
    camera(0, 0, 1000); rtps(1, 1, 1); swap(); vblanks(1);
    camera(0, 0, 1000); rtps(1, 1, 1);
    frame_identity_reset();
    camera(0, 0, 1000); rtps(1, 1, 1); swap(); vblanks(1);
    camera(0, 0, 1000); rtps(1, 1, 1); swap();
}

static void scenario_corners(void) {
    static const int tri[9] = {0, 0, 0, 100, 0, 0, 0, 100, 0}, other[9] = {500, 500, 0, 600, 500, 0, 500, 600, 0};
    uint32_t a[3], b[3], d[3], e[3], loose;
    start_service(0);
    camera(0, 0, 1000); rtpt(tri, a); rtpt(other, d);
    swap(); vblanks(2);
    camera(2, 0, 1000); rtpt(tri, a); rtpt(other, d);
    swap(); vblanks(2);
    /* Frame 2: the model moved again; frame 1's ordering table is drawn after
     * the change (age 1), then this frame's own polygons (age 0). */
    triangle(a[0], a[1], a[2]);
    camera(4, 0, 1000); rtpt(tri, b); rtpt(other, e);
    gouraud(b[0], b[1], b[2]);
    quad((const uint32_t[4]){e[0], e[1], e[2], e[2]});
    triangle(0x00100010u, 0x00200020u, 0x00100020u);          /* 2D: no projection */
    triangle(b[0], 0x00300030u, 0x00100030u);                 /* one corner only: split */
    camera(9, 0, 1000);
    loose = rtps(900, 900, 0);                                 /* a new run: unpaired */
    triangle(b[0], b[1], loose);
    swap(); vblanks(2);
    /* Frame 3: a polygon drawn before its projections exist this frame must
     * not take them; it takes frame 2's, where they stood. */
    triangle(b[0], b[1], b[2]);
    camera(4, 0, 1000); rtpt(tri, b);
    swap();
}

static void scenario_shared(void) {
    /* Two runs project to one screen point. Where both stood together in the
     * previous frame the corner is safe; where they came apart, only the
     * polygon's own run can decide it. */
    static const int left[9] = {0, 0, 0, 100, 0, 0, 0, 100, 0}, right[9] = {0, 0, 0, 100, 0, 0, 100, 100, 0};
    uint32_t a[3], b[3];
    start_service(0);
    camera(0, 0, 1000); rtpt(left, a); camera(0, 0, 1000); c->gte_ctrl[1] = 1; rtpt(right, b);
    swap(); vblanks(2);
    camera(0, 0, 1000); rtpt(left, a); camera(0, 0, 1000); c->gte_ctrl[1] = 1; rtpt(right, b);
    triangle(a[0], a[1], a[2]);                                /* together before: safe */
    swap(); vblanks(2);
    camera(0, 0, 1000); rtpt(left, a); camera(30, 0, 1000); c->gte_ctrl[1] = 1; rtpt(right, b);
    swap(); vblanks(2);
    /* Now both start at the same point again, but stood 30 units apart. */
    camera(0, 0, 1000); rtpt(left, a); camera(0, 0, 1000); c->gte_ctrl[1] = 1; rtpt(right, b);
    triangle(a[0], a[1], a[2]);                                /* only run 0 has all corners */
    triangle(a[0], a[1], a[1]);                                /* both runs have all: held */
    triangle(a[0], 0x01230123u, a[2]);                         /* no single frame: split */
    swap();
}

static void scenario_excluded(void) {
    static const int tri[9] = {0, 0, 0, 100, 0, 0, 0, 100, 0};
    uint32_t a[3];
    start_service(0);
    camera(0, 0, 1000); rtpt(tri, a); swap(); vblanks(2);
    exclude = 2 | FRAME_IDENTITY_EXCLUDE;
    camera(2, 0, 1000); rtpt(tri, a); triangle(a[0], a[1], a[2]); swap(); vblanks(2);
    exclude = 0;
    camera(4, 0, 1000); rtpt(tri, a); triangle(a[0], a[1], a[2]); swap();
}

/* The recording entries leave the GTE exactly as the plain ones do. */
static void scenario_entries(void) {
    static const uint32_t ops[] = {(1u << 19) | 0x01, 0x30, (1u << 19) | (1u << 10) | 0x30, 0x06, (1u << 19) | 0x12, 0x2d};
    cpu_t *plain = calloc(1, sizeof(*plain));
    unsigned i, k;
    start_service(0);
    for (i = 0; i < sizeof(ops) / sizeof(ops[0]); ++i) {
        for (k = 0; k < 32; ++k) {
            c->gte_data[k] = (uint32_t)rand() * 2654435761u;
            c->gte_ctrl[k] = (uint32_t)rand() * 40503u;
        }
        c->gte_ctrl[26] &= 0xffff;
        memcpy(plain->gte_data, c->gte_data, sizeof(c->gte_data));
        memcpy(plain->gte_ctrl, c->gte_ctrl, sizeof(c->gte_ctrl));
        plain->gte_commands = c->gte_commands;
        frame_identity_gte_command(c, ops[i]);
        gte_cmd(plain, ops[i]);
        check(!memcmp(plain->gte_data, c->gte_data, sizeof(c->gte_data)) &&
              !memcmp(plain->gte_ctrl, c->gte_ctrl, sizeof(c->gte_ctrl)) && plain->gte_commands == c->gte_commands,
              "recording entry changed the GTE result");
    }
    check(frame_identity_totals().projections == 7, "projection count");
    free(plain);
    printf("entries %s\n", failures ? "differ" : "match");
}

int main(int argc, char **argv) {
    const char *name = argc > 1 ? argv[1] : "";
    c = calloc(1, sizeof(*c));
    g = gpu_create();
    if (!c || !g) return 2;
    if (!strcmp(name, "runs")) scenario_runs();
    else if (!strcmp(name, "repeats")) scenario_repeats(0);
    else if (!strcmp(name, "repeats-unguarded")) scenario_repeats(100000);
    else if (!strcmp(name, "gaps")) scenario_gaps();
    else if (!strcmp(name, "corners")) scenario_corners();
    else if (!strcmp(name, "shared")) scenario_shared();
    else if (!strcmp(name, "excluded")) scenario_excluded();
    else if (!strcmp(name, "entries")) scenario_entries();
    else { fprintf(stderr, "unknown scenario %s\n", name); return 2; }
    frame_identity_stop();
    free(c);
    return failures ? 1 : 0;
}
