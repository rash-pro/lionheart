/* Drives the frame identity service, frame blending and the recorder with a
 * synthetic double-buffered scene, no game data, and prints what blending
 * gives for each game frame:
 *   F<number> reason=<why> moving=<corners>/<3D corners> polygons=<moving>/<3D> held=<service>/<projection>/<neighbour>/<reset>
 *   M<number> <polygon>=<mask of the corners that move at alpha 1/2>...
 *   E<number> ends=<exact|differ> shared=<kept|split>
 *   T<number> turning=<corners> runs=<turning>/<hanging>/<flat or mirrored>/<too far>
 *   S<number> resized=<polygons> stray=<corners> size=<smallest>-<largest edge, in percent of the ends'>
 *   R<number> resolved=<ambiguous corners resolved by the polygon drawn a game frame earlier>
 *   L<number> leftovers=<polygons of the frame before culled at the screen's edge, drawn between>
 *   W<number> following=<corners projected anew that follow the polygons around them>
 *   C<number> <polygon>=<mask of the corners whose colour changes>[/<first corner's colour at alpha 0, 1/2 and 1>]...
 * Scenarios: move, held, touch, resets, late; a polygon the GPU skips:
 * hidden; sliding far, with C held or skipped: slide, slide-hidden; parts that turn: turn, chain,
 * far, flat, mirror, mirrored, stretched; two vertices of a part that meet
 * on screen: cross; colours that change: fade; a strip panning off the screen,
 * its polygons culled once wholly off it: pan; a shadow-like fan whose centre
 * is projected anew each game frame, sliding or torn apart: shadow, torn. With "straight" after the name, every corner moves on a
 * straight line.
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

static double length(uint32_t a, uint32_t b) {
    double x = (double)(int16_t)(a & 0xffffu) - (int16_t)(b & 0xffffu), y = (double)(int16_t)(a >> 16) - (int16_t)(b >> 16), r = x * x + y * y, n = r > 1 ? r : 1;
    unsigned i;
    for (i = 0; i < 64; ++i) n = (n + r / n) / 2;
    return r > 0 ? n : 0;
}

/* Every quad's edges on screen at alpha 1/4, 1/2 and 3/4 against the same
 * edges at the two ends, and the blender's own checks. A part seen face on
 * keeps its size on screen as it turns about the line of sight. */
static void sizes(const frame_identity_frame_t *f) {
    static const unsigned edges[4][2] = {{0, 1}, {1, 3}, {3, 2}, {2, 0}};
    frame_blend_check_t check = {0, 0, 0};
    double low = 1e9, high = 0;
    uint32_t i, k, e;
    for (i = 0; i < f->polygon_count; ++i) {
        uint32_t zero[4], one[4];
        if (f->polygons[i].corners != 4) continue;
        memcpy(zero, f->polygons[i].xy, sizeof(zero)); memcpy(one, f->polygons[i].xy, sizeof(one));
        if (frame_blend_polygon(blend, f->first_polygon + i, 0, 4, zero, NULL) != 15) continue;
        for (k = 1; k < 4; ++k) {
            uint32_t at[4];
            memcpy(at, f->polygons[i].xy, sizeof(at));
            frame_blend_polygon(blend, f->first_polygon + i, k, 4, at, &check);
            for (e = 0; e < 4; ++e) {
                double l = length(at[edges[e][0]], at[edges[e][1]]), l0 = length(zero[edges[e][0]], zero[edges[e][1]]),
                       l1 = length(one[edges[e][0]], one[edges[e][1]]), r = l / ((l0 + l1) / 2);
                if (r < low) low = r;
                if (r > high) high = r;
            }
        }
    }
    if (high > 0)
        printf("S%llu resized=%u stray=%u size=%.0f-%.0f\n", (unsigned long long)f->number, check.resized, check.stray, low * 100, high * 100);
}

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
    printf("T%llu turning=%u runs=%u/%u/%u/%u\n", (unsigned long long)f->number, info->corners_turning, info->runs_turning,
           info->runs_attached, info->runs_flat, info->runs_far);
    if (info->corners_resolved) printf("R%llu resolved=%u\n", (unsigned long long)f->number, info->corners_resolved);
    if (info->leftovers) printf("L%llu leftovers=%u\n", (unsigned long long)f->number, info->leftovers);
    if (info->corners_following) printf("W%llu following=%u\n", (unsigned long long)f->number, info->corners_following);
    if (!f->polygon_count) return;
    sizes(f);
    {
        unsigned n = f->polygon_count < 3 ? f->polygon_count : 3;
        printf("C%llu", (unsigned long long)f->number);
        for (i = 0; i < n; ++i) {
            uint32_t zero[4], half[4], one[4];
            unsigned m = frame_blend_colours(blend, f->first_polygon + i, 1, 2, half);
            printf(" %c=%x", 'A' + i, m);
            if (m) {
                frame_blend_colours(blend, f->first_polygon + i, 0, 2, zero);
                frame_blend_colours(blend, f->first_polygon + i, 2, 2, one);
                printf("/%06x-%06x-%06x", (unsigned)zero[0], (unsigned)half[0], (unsigned)one[0]);
            }
        }
        printf("\n");
    }
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

/* A polygon packet to the GPU, told to blending as the GPU takes it. */
static uint64_t polygons_drawn;
static void polygon(const uint32_t *words, unsigned n) {
    unsigned i;
    frame_blend_packet(blend, polygons_drawn++, words);
    for (i = 0; i < n; ++i) gpu_gp0(g, words[i]);
}

static void quad(uint32_t color, const uint32_t xy[4]) {
    uint32_t w[5] = {0x28000000u | color, xy[0], xy[1], xy[2], xy[3]};
    polygon(w, 5);
}

static void triangle(uint32_t color, uint32_t a, uint32_t b_, uint32_t c_) {
    uint32_t w[4] = {0x20000000u | color, a, b_, c_};
    polygon(w, 4);
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
        triangle(0xc00000u, drawn.b[1], drawn.b[3], (200u << 16) | 300u);
    } else if (with_c == 3) {
        /* C's third corner far below the screen, where the GTE clamps: the
         * GPU draws none of it. */
        triangle(0xc00000u, drawn.b[1], drawn.b[3], (1023u << 16) | 300u);
    } else if (with_c == 2) {
        triangle(0xc0c000u, drawn.a[0], (200u << 16) | 10u, (220u << 16) | 60u);
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

/* The same, sliding far each game frame. */
static void scenario_slide(int with_c) {
    int f;
    for (f = 0; f < 8; ++f) game_frame(-160 + f * 60, 900, 0, with_c, 2);
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

/* Sine and cosine of degrees, by their series. */
static double sine(double degrees) {
    double x, term, sum;
    unsigned n;
    while (degrees > 180) degrees -= 360;
    while (degrees < -180) degrees += 360;
    x = degrees * 3.14159265358979323846 / 180; term = x; sum = x;
    for (n = 1; n < 20; ++n) { term *= -x * x / ((2 * n) * (2 * n + 1)); sum += term; }
    return sum;
}
static double cosine(double degrees) { return sine(degrees + 90); }

static int16_t fixed(double v) { return (int16_t)(v >= 0 ? v * 4096 + 0.5 : v * 4096 - 0.5); }

/* A rotation about the line of sight, times scale (sx, sy, sz) along the
 * model's own axes, and a translation. */
static void pose(double degrees, double sx, double sy, double sz, int tx, int ty, int tz) {
    double c0 = cosine(degrees), s0 = sine(degrees);
    int16_t r[9];
    r[0] = fixed(c0 * sx); r[1] = fixed(-s0 * sy); r[2] = 0;
    r[3] = fixed(s0 * sx); r[4] = fixed(c0 * sy); r[5] = 0;
    r[6] = 0; r[7] = 0; r[8] = fixed(sz);
    c->gte_ctrl[0] = (uint16_t)r[0] | (uint32_t)(uint16_t)r[1] << 16;
    c->gte_ctrl[1] = (uint16_t)r[2] | (uint32_t)(uint16_t)r[3] << 16;
    c->gte_ctrl[2] = (uint16_t)r[4] | (uint32_t)(uint16_t)r[5] << 16;
    c->gte_ctrl[3] = (uint16_t)r[6] | (uint32_t)(uint16_t)r[7] << 16;
    c->gte_ctrl[4] = (uint16_t)r[8];
    c->gte_ctrl[5] = (uint32_t)tx; c->gte_ctrl[6] = (uint32_t)ty; c->gte_ctrl[7] = (uint32_t)tz;
    c->gte_ctrl[24] = 160u << 16; c->gte_ctrl[25] = 120u << 16; c->gte_ctrl[26] = 300;
}

static void shown(void) {
    unsigned i;
    gpu_gp1(g, 0x05000000u | (back * 256u) << 10);
    back ^= 1;
    for (i = 0; i < 4; ++i) gpu_vblank(g);
}

static void cleared(void) {
    draw_area(back);
    gpu_gp0(g, 0x02202020u); gpu_gp0(g, (back * 256u) << 16); gpu_gp0(g, 240u << 16 | 320u);
}

/* A square part, 200 units across, turning about the line of sight by step
 * degrees each game frame, at scale sx along its own x (negative: mirrored);
 * mirror_first mirrors only the first frame. */
static void scenario_turn(double step, double sx, double sy, int mirror_first) {
    static const int corner[4][2] = {{-100, -100}, {100, -100}, {-100, 100}, {100, 100}};
    int f;
    for (f = 0; f < 6; ++f) {
        uint32_t xy[4];
        unsigned i;
        cleared();
        pose(step * f, f == 0 && mirror_first ? -sx : sx, sy, 1, 10 * f, 0, 1000);
        for (i = 0; i < 4; ++i) xy[i] = rtps(corner[i][0], corner[i][1], 0);
        quad(0x0000c0u, xy);
        shown();
    }
}

/* Two rods, A from its origin along x and a thinner B hanging from A's far
 * end, each its own run: A turns 40 degrees a game frame, B 30 more against
 * A. B's first corners are A's last ones, projected in both runs. */
static void scenario_chain(void) {
    int f;
    for (f = 0; f < 6; ++f) {
        static const int rod[4][2] = {{0, -40}, {200, -40}, {0, 40}, {200, 40}}, tip[4][2] = {{0, -40}, {150, -30}, {0, 40}, {150, 30}};
        uint32_t a[4], bq[4];
        double turn = 40.0 * f, bend = 30.0 * f;
        int tx = -150 + 5 * f, tz = 700, bx, by;
        unsigned i;
        cleared();
        pose(turn, 1, 1, 1, tx, 0, tz);
        for (i = 0; i < 4; ++i) a[i] = rtps(rod[i][0], rod[i][1], 0);
        /* B's origin is A's (200, 0, 0), as the game would compute it. */
        bx = tx + (int)((fixed(cosine(turn)) * 200) >> 12); by = (int)((fixed(sine(turn)) * 200) >> 12);
        pose(turn + bend, 1, 1, 1, bx, by, tz);
        for (i = 0; i < 4; ++i) bq[i] = rtps(tip[i][0], tip[i][1], 0);
        /* B's first corners are drawn where A put them. */
        bq[0] = a[1]; bq[2] = a[3];
        quad(0x0000c0u, a);
        quad(0x00c000u, bq);
        shown();
    }
}

/* One part sliding across the line of sight: a quad with a corner at its
 * origin and, 300 units behind, a triangle with a corner there too. When the
 * origin crosses the line of sight both corners land on the centre of the
 * screen, having stood apart a game frame earlier: the service cannot tell
 * which is which there. */
static void scenario_cross(void) {
    static const int front[4][3] = {{0, 0, 0}, {100, 0, 0}, {0, 100, 0}, {100, 100, 0}};
    static const int back_[3][3] = {{0, 0, 300}, {-100, 0, 300}, {0, -100, 300}};
    int f;
    for (f = 0; f < 6; ++f) {
        uint32_t q[4], t[3];
        unsigned i;
        cleared();
        pose(0, 1, 1, 1, -60 + 20 * f, 0, 900);
        for (i = 0; i < 4; ++i) q[i] = rtps(front[i][0], front[i][1], front[i][2]);
        for (i = 0; i < 3; ++i) t[i] = rtps(back_[i][0], back_[i][1], back_[i][2]);
        quad(0x0000c0u, q);
        triangle(0xc00000u, t[0], t[1], t[2]);
        shown();
    }
}

/* Three quads that move a little each game frame while their look changes:
 * A shaded per corner, fading up; B flat, fading down; C textured and
 * semi-transparent, its colour changing and its blending mode with it. */
static void scenario_fade(void) {
    static const int corner[4][2] = {{-100, -60}, {100, -60}, {-100, 60}, {100, 60}};
    int f;
    for (f = 0; f < 6; ++f) {
        uint32_t xy[3][4], level = 40u * (uint32_t)f;
        unsigned i, k;
        cleared();
        for (k = 0; k < 3; ++k) {
            pose(10.0 * f, 1, 1, 1, -250 + 250 * (int)k, 0, 1000);
            /* A different shape each, so that each quad is its own run. */
            for (i = 0; i < 4; ++i) xy[k][i] = rtps(corner[i][0] + (int)k, corner[i][1], 0);
        }
        {
            uint32_t a[8] = {0x38000000u | level, xy[0][0], 0x00ff00u | level << 16, xy[0][1], 0x0000ffu, xy[0][2],
                             0x404040u + level, xy[0][3]};
            uint32_t bq[5] = {0x28000000u | (200u - level) << 8, xy[1][0], xy[1][1], xy[1][2], xy[1][3]};
            /* Texture page 0 with blending mode f & 1 (bits 5-6), palette at 0,480. */
            uint32_t cq[9] = {0x2e000000u | level, xy[2][0], 0x78000000u, xy[2][1], ((uint32_t)(f & 1) << 5) << 16 | 0x0f00u,
                              xy[2][2], 0x0f0fu, xy[2][3], 0x000fu};
            polygon(a, 8); polygon(bq, 5); polygon(cq, 9);
        }
        shown();
    }
}

/* A strip of ten quads, one run, sliding left 20 pixels a game frame; as a
 * game does, quads wholly off the screen are not drawn. */
static void scenario_pan(void) {
    int f;
    for (f = 0; f < 8; ++f) {
        uint32_t top[11], bottom[11];
        unsigned i;
        cleared();
        pose(0, 1, 1, 1, -80 * f, 0, 1200);
        for (i = 0; i <= 10; ++i) { top[i] = rtps(-600 + 240 * (int)i, 0, 0); bottom[i] = rtps(-600 + 240 * (int)i, 240, 0); }
        for (i = 0; i < 10; ++i) {
            uint32_t xy[4] = {top[i], top[i + 1], bottom[i], bottom[i + 1]};
            int16_t right = (int16_t)(top[i + 1] & 0xffffu), left = (int16_t)(top[i] & 0xffffu);
            if (right < 0 || left >= 320) continue;
            quad(i & 1 ? 0x0000c0u : 0x00c000u, xy);
        }
        shown();
    }
}

/* A fan of eight triangles around a centre projected anew each game frame
 * (its input changes, so it never pairs), the ring its own run, sliding 20
 * pixels a game frame; torn, the ring also spreads far each game frame. */
static void scenario_shadow(int torn) {
    int f;
    for (f = 0; f < 6; ++f) {
        uint32_t ring[8], centre;
        unsigned i;
        double spread = torn ? 1 + 0.6 * f : 1;
        cleared();
        pose(0, spread, spread, 1, -200 + 80 * f, 100, 1200);
        for (i = 0; i < 8; ++i) ring[i] = rtps((int)(200 * cosine(45.0 * i)), (int)(100 * sine(45.0 * i)), 0);
        pose(0, 1, 1, 1, -200 + 80 * f, 100, 1200);
        c->gte_ctrl[2] |= 1u << 16;
        centre = rtps(0, 0, f);
        for (i = 0; i < 8; ++i) triangle(0x606060u, centre, ring[i], ring[(i + 1) % 8]);
        shown();
    }
}

int main(int argc, char **argv) {
    const char *name = argc > 1 ? argv[1] : "", *record = argc > 2 && strcmp(argv[2], "straight") ? argv[2] : NULL;
    frame_identity_config_t config;
    c = calloc(1, sizeof(*c));
    g = gpu_create();
    blend = frame_blend_create();
    if (!c || !g || !blend) return 2;
    if (argc > 2 && !strcmp(argv[2], "straight")) frame_blend_straight(blend, 1);
    display();
    memset(&config, 0, sizeof(config));
    config.sink = sink; config.classify = classify;
    frame_identity_start(g, &config);
    gpu_gp1(g, 0x05000000u);                                  /* the first start it sees */
    if (record && !frame_record_start(g, record, 1, 0)) { fprintf(stderr, "cannot record to %s\n", record); return 2; }
    if (!strcmp(name, "move")) scenario_move(0);
    else if (!strcmp(name, "held")) scenario_move(1);
    else if (!strcmp(name, "touch")) scenario_move(2);
    else if (!strcmp(name, "hidden")) scenario_move(3);
    else if (!strcmp(name, "slide")) scenario_slide(1);
    else if (!strcmp(name, "slide-hidden")) scenario_slide(3);
    else if (!strcmp(name, "resets")) scenario_resets();
    else if (!strcmp(name, "late")) scenario_late();
    else if (!strcmp(name, "turn")) scenario_turn(90, 1, 1, 0);
    else if (!strcmp(name, "far")) scenario_turn(175, 1, 1, 0);
    else if (!strcmp(name, "flat")) scenario_turn(90, 1, 0.001, 0);
    else if (!strcmp(name, "mirror")) scenario_turn(60, 1, 1, 1);
    else if (!strcmp(name, "mirrored")) scenario_turn(60, -1, 1, 0);
    else if (!strcmp(name, "stretched")) scenario_turn(60, 2, 0.5, 0);
    else if (!strcmp(name, "chain")) scenario_chain();
    else if (!strcmp(name, "cross")) scenario_cross();
    else if (!strcmp(name, "fade")) scenario_fade();
    else if (!strcmp(name, "pan")) scenario_pan();
    else if (!strcmp(name, "shadow")) scenario_shadow(0);
    else if (!strcmp(name, "torn")) scenario_shadow(1);
    else { fprintf(stderr, "unknown scenario %s\n", name); return 2; }
    gpu_gp1(g, 0x05000000u | (back * 256u) << 10);
    if (record && !frame_record_stop()) { printf("FAIL recording\n"); ++failures; }
    frame_identity_stop();
    frame_blend_destroy(blend);
    free(c);
    return failures ? 1 : 0;
}
