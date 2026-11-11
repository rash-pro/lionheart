/* Drives the frame identity service, frame blending and the recorder with a
 * synthetic double-buffered scene, no game data, and prints what blending
 * gives for each game frame:
 *   F<number> reason=<why> moving=<corners>/<3D corners> polygons=<moving>/<3D> held=<service>/<projection>/<neighbour>/<reset>
 *   M<number> <polygon>=<mask of the corners that move at alpha 1/2>...
 *   E<number> ends=<exact|differ> shared=<kept|split>
 * Scenarios: move, held, touch, resets, late.
 * Usage: frame_blend_harness SCENARIO [RECORDING] */
#include "frame_blend.h"
#include "frame_identity.h"
#include "frame_record.h"
#include "gpu.h"
#include "gte.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static cpu_t *c;
static gpu_t *g;
static frame_blend_t *blend;
static unsigned back = 1, tag = 1;
static int failures;

static const char *const reasons[FRAME_BLEND_REASONS] = {"kept", "no-history", "excluded", "scene", "cut"};

/* Two quads side by side (A and B), each projected as its own transform run,
 * so the two corners they share are two projections at one position; and a
 * triangle C over B's right edge with one corner the GTE never projected. */
static struct {
    uint32_t a[4], b[4];
} drawn;

static uint32_t polygon_words[3][4];

static void sink(void *context, const frame_identity_frame_t *f) {
    const frame_blend_info_t *info;
    uint32_t i, k;
    (void)context;
    frame_record_frame(f);
    if (!frame_blend_add(blend, f)) { printf("FAIL out of memory\n"); ++failures; return; }
    info = frame_blend_info(blend);
    printf("F%llu reason=%s moving=%u/%u polygons=%u/%u held=%u/%u/%u/%u\n", (unsigned long long)f->number,
           reasons[info->reason], info->corners_moving, info->corners_3d, info->polygons_moving, info->polygons_3d,
           info->held_service, info->held_projection, info->held_neighbour, info->held_reset);
    if (!f->polygon_count) return;
    {
        uint32_t half[3][4], zero[3][4], one[3][4];
        unsigned mask[3], m0, m1, n = f->polygon_count < 3 ? f->polygon_count : 3, exact = 1;
        printf("M%llu", (unsigned long long)f->number);
        for (i = 0; i < n; ++i) {
            for (k = 0; k < 4; ++k) half[i][k] = zero[i][k] = one[i][k] = f->polygons[i].xy[k];
            mask[i] = frame_blend_polygon(blend, f->first_polygon + i, 1, 2, half[i], NULL);
            m0 = frame_blend_polygon(blend, f->first_polygon + i, 0, 4, zero[i], NULL);
            m1 = frame_blend_polygon(blend, f->first_polygon + i, 4, 4, one[i], NULL);
            printf(" %c=%x", 'A' + i, mask[i]);
            /* Alpha 1 is where the corner is now; alpha 0 where its
             * projection's pair stood: for this scene, the frame before's. */
            for (k = 0; k < f->polygons[i].corners; ++k) {
                if (m1 != mask[i] || m0 != mask[i]) exact = 0;
                if ((m1 >> k & 1) && one[i][k] != f->polygons[i].xy[k]) exact = 0;
                if ((m0 >> k & 1) && zero[i][k] != polygon_words[i][k]) exact = 0;
            }
        }
        printf("\n");
        /* A's right corners are B's left ones. */
        printf("E%llu ends=%s shared=%s\n", (unsigned long long)f->number, exact ? "exact" : "differ",
               n >= 2 && half[0][1] == half[1][0] && half[0][3] == half[1][2] ? "kept" : "split");
        for (i = 0; i < n; ++i) for (k = 0; k < 4; ++k) polygon_words[i][k] = f->polygons[i].xy[k];
    }
}

static uint32_t classify(void *context, const cpu_t *cpu) { (void)context; (void)cpu; return tag; }

/* Rotation about Y by a small angle (1.0 is 4096), translation, H and centre. */
static void camera(int tx, int ty, int tz, int turn) {
    unsigned i;
    for (i = 0; i < 5; ++i) c->gte_ctrl[i] = 0;
    c->gte_ctrl[0] = 4096; c->gte_ctrl[4] = 4096;
    c->gte_ctrl[1] = (uint32_t)(uint16_t)turn;              /* R13 */
    c->gte_ctrl[3] = (uint32_t)(uint16_t)(-turn) | 0u;      /* R31 */
    c->gte_ctrl[2] = 4096;                                  /* R22 */
    c->gte_ctrl[5] = (uint32_t)tx; c->gte_ctrl[6] = (uint32_t)ty; c->gte_ctrl[7] = (uint32_t)tz;
    c->gte_ctrl[24] = 160u << 16; c->gte_ctrl[25] = 120u << 16; c->gte_ctrl[26] = 300;
}

static uint32_t rtps(int x, int y, int z) {
    c->gte_data[0] = (uint32_t)(uint16_t)x | (uint32_t)(uint16_t)y << 16;
    c->gte_data[1] = (uint32_t)(uint16_t)z;
    frame_identity_gte_command(c, (1u << 19) | 0x01);
    return c->gte_data[14];
}

static void draw_area(unsigned buffer) {
    unsigned y = buffer * 256;
    gpu_gp0(g, 0xe3000000u | y << 10);
    gpu_gp0(g, 0xe4000000u | 319u | (y + 239) << 10);
    gpu_gp0(g, 0xe5000000u | y << 11);
}

static void quad(uint32_t color, const uint32_t xy[4]) {
    unsigned i;
    gpu_gp0(g, 0x28000000u | color);
    for (i = 0; i < 4; ++i) gpu_gp0(g, xy[i]);
}

static void display(void) {
    gpu_gp1(g, 0x08000001u);                                 /* 320 wide, 15-bit */
    gpu_gp1(g, 0x06000000u | 0x260u | (0x260u + 2560u) << 12);
    gpu_gp1(g, 0x07000000u | 16u | (16u + 240u) << 10);
    gpu_gp1(g, 0x05000000u);
    gpu_gp1(g, 0x03000000u);
}

/* One game frame: clear the back buffer, project and draw the scene, show it,
 * draw the HUD into the picture now shown, then let VBlanks pass. */
/* With C 1, also C over B's right edge; with C 2, a triangle that only touches
 * A at one corner, its others never projected. */
static void game_projections(int tx, int tz, int turn) {
    static const int qa[4][2] = {{0, 0}, {100, 0}, {0, 100}, {100, 100}}, qb[4][2] = {{100, 0}, {200, 0}, {100, 100}, {200, 100}};
    unsigned i;
    camera(tx, 0, tz, turn);
    for (i = 0; i < 4; ++i) drawn.a[i] = rtps(qa[i][0] - 100, qa[i][1] - 50, 0);
    /* B's own run: a different setup (R23 meets only z, which is 0 here), the same place. */
    camera(tx, 0, tz, turn); c->gte_ctrl[2] |= 1u << 16;
    for (i = 0; i < 4; ++i) drawn.b[i] = rtps(qb[i][0] - 100, qb[i][1] - 50, 0);
}

static void game_frame(int tx, int tz, int turn, int with_c, unsigned vblanks) {
    unsigned i;
    draw_area(back);
    gpu_gp0(g, 0x02202020u); gpu_gp0(g, (back * 256u) << 16); gpu_gp0(g, 240u << 16 | 320u);
    game_projections(tx, tz, turn);
    quad(0x0000c0u, drawn.a);
    quad(0x00c000u, drawn.b);
    if (with_c == 1) {
        gpu_gp0(g, 0x20c00000u);
        gpu_gp0(g, drawn.b[1]); gpu_gp0(g, drawn.b[3]); gpu_gp0(g, (200u << 16) | 300u);
    } else if (with_c == 2) {
        gpu_gp0(g, 0x20c0c000u);
        gpu_gp0(g, drawn.a[0]); gpu_gp0(g, (200u << 16) | 10u); gpu_gp0(g, (220u << 16) | 60u);
    }
    gpu_gp1(g, 0x05000000u | (back * 256u) << 10);
    draw_area(back);
    gpu_gp0(g, 0x60ffffffu); gpu_gp0(g, (8u << 16) | 8u); gpu_gp0(g, (8u << 16) | 40u);   /* HUD */
    back ^= 1;
    for (i = 0; i < vblanks; ++i) gpu_vblank(g);
}

/* As battle draws: each game frame draws, after the display start changed,
 * the polygons the CPU projected in the frame before, then projects its own. */
static void late_frame(int tx, int tz, int turn) {
    static uint32_t a[4], b[4];
    static int pending;
    unsigned i;
    draw_area(back);
    gpu_gp0(g, 0x02202020u); gpu_gp0(g, (back * 256u) << 16); gpu_gp0(g, 240u << 16 | 320u);
    if (pending) { quad(0x0000c0u, a); quad(0x00c000u, b); }
    game_projections(tx, tz, turn);
    memcpy(a, drawn.a, sizeof(a)); memcpy(b, drawn.b, sizeof(b));
    pending = 1;
    gpu_gp1(g, 0x05000000u | (back * 256u) << 10);
    back ^= 1;
    for (i = 0; i < 2; ++i) gpu_vblank(g);
}

/* A cut in the projections shows a game frame later: the frame that cuts
 * still draws, and blends, those of the frame before. */
static void scenario_late(void) {
    int f;
    for (f = 0; f < 4; ++f) late_frame(f * 6, 900, 0);
    for (f = 0; f < 4; ++f) late_frame(300 + f * 6, 2400, 900);
}

static void scenario_move(int with_c) {
    int f;
    for (f = 0; f < 8; ++f) game_frame(-40 + f * 6, 900 - f * 15, f * 40, with_c, 2);
}

static void scenario_resets(void) {
    int f;
    for (f = 0; f < 3; ++f) game_frame(f * 6, 900, 0, 0, 2);
    /* A cut: the camera is somewhere else at once. */
    game_frame(300, 2400, 900, 0, 2);
    game_frame(306, 2400, 900, 0, 2);
    /* The scene changes. */
    tag = 3;
    game_frame(312, 2400, 900, 0, 2);
    game_frame(318, 2400, 900, 0, 12);
    /* After a gap. */
    game_frame(324, 2400, 900, 0, 2);
    game_frame(330, 2400, 900, 0, 2);
}

int main(int argc, char **argv) {
    const char *name = argc > 1 ? argv[1] : "", *record = argc > 2 ? argv[2] : NULL;
    frame_identity_config_t config;
    c = calloc(1, sizeof(*c));
    g = gpu_create();
    blend = frame_blend_create();
    if (!c || !g || !blend) return 2;
    display();
    memset(&config, 0, sizeof(config));
    config.sink = sink; config.classify = classify;
    frame_identity_start(g, &config);
    gpu_gp1(g, 0x05000000u);                                  /* the first start it sees */
    if (record && !frame_record_start(g, record, 1, 0)) { fprintf(stderr, "cannot record to %s\n", record); return 2; }
    if (!strcmp(name, "move")) scenario_move(0);
    else if (!strcmp(name, "held")) scenario_move(1);
    else if (!strcmp(name, "touch")) scenario_move(2);
    else if (!strcmp(name, "resets")) scenario_resets();
    else if (!strcmp(name, "late")) scenario_late();
    else { fprintf(stderr, "unknown scenario %s\n", name); return 2; }
    gpu_gp1(g, 0x05000000u | (back * 256u) << 10);
    if (record && !frame_record_stop()) { printf("FAIL recording\n"); ++failures; }
    frame_identity_stop();
    frame_blend_destroy(blend);
    free(c);
    return failures ? 1 : 0;
}
