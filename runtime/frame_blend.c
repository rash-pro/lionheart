#include "frame_blend.h"
#include "gte_divide.h"
#include <stdlib.h>
#include <string.h>

/* What blending needs of one identity frame, kept for the two after it:
 * a polygon corner of age 1 is a projection of the frame before, and its
 * pair is in the frame before that. */
typedef struct {
    uint64_t number;
    uint32_t tag, reason;
    int valid;
    frame_identity_projection_t *projections;
    frame_identity_run_t *runs;
    frame_identity_setup_t *setups;
    uint8_t *jumped;             /* per run: it jumped, by the cut rule applied to it alone */
    struct motion *motions;      /* per run: how it moves from the frame before */
    /* Its polygons, with the corners blending resolved, and an index of
     * them by linked corner: (corner, age, projection) to the first polygon,
     * then per polygon corner the next with the same. */
    frame_identity_polygon_t *polygons;
    /* Each polygon's corner colours as its packet gave them (one for a flat
     * polygon), the texture page word of a textured one, and whether its
     * packet was seen. */
    uint32_t (*colours)[4];
    uint32_t (*packets)[12];
    uint32_t *pages;
    uint8_t *coloured;
    uint64_t *corner_keys;
    int32_t *corner_heads, *corner_next;
    size_t projection_count, run_count, setup_count, polygon_count;
    size_t projection_capacity, run_capacity, setup_capacity, jumped_capacity, motion_capacity, polygon_capacity;
    size_t corner_capacity, next_capacity, colour_capacity, page_capacity, coloured_capacity, packet_capacity;
} kept_t;

/* A polygon packet as the GPU took it, waiting for its identity frame. */
typedef struct {
    uint64_t serial;
    uint32_t opcode, page;
    uint32_t colours[4];
    uint32_t words[12];
} look_t;

/* A polygon of the frame before that the latest no longer draws, having left
 * the screen: its packet and its corners' vertices. */
typedef struct {
    uint32_t words[12];
    int32_t vertex[4];
} leftover_t;

/* Both ends of a vertex: where it stood in the frame before (a) and now (b). */
typedef struct {
    int32_t view[2][3];
    int32_t ofx[2], ofy[2];
    uint32_t h[2], sxy[2];
    uint32_t shift, lm;
    int pinned;
    /* A vertex of a run that turns follows the run's motion (age, run and
     * its model-space input); otherwise, run is -1 and it moves on a
     * straight line in view space. */
    int32_t run;
    uint8_t age;
    int16_t input[3];
} vertex_t;

struct frame_blend {
    kept_t kept[3];
    unsigned latest;             /* index in kept of the newest frame */
    frame_blend_info_t info;
    /* The latest frame's polygons: per corner, a vertex or -1. */
    int32_t (*corners)[4];
    uint8_t (*codes)[4];
    size_t corner_capacity, code_capacity;
    vertex_t *vertices;
    size_t vertex_count, vertex_capacity;
    /* Packet position to vertex, open addressing. */
    uint64_t *keys;
    int32_t *slots;
    size_t table_capacity;
    uint8_t *hidden;             /* per polygon: the GPU draws none of it */
    size_t hidden_capacity;
    int32_t *surfaces;           /* per polygon: union-find over shared edges */
    size_t surface_capacity;
    double *moves;               /* four arrays of one value per run */
    uint64_t histogram[2048];
    size_t move_capacity;
    int straight;                /* every corner on a straight line, for comparison */
    look_t *looks;               /* packets not yet in a frame, in serial order */
    size_t look_count, look_capacity;
    /* The latest frame's polygons: the same polygon a game frame earlier, by
     * index in the frame before, or -1. */
    int32_t *before;
    size_t before_capacity;
    /* Polygons of the frame before with a successor in the latest; where the
     * projections of the frames before stand a game frame later, by index;
     * and what the latest frame no longer draws. */
    uint8_t *continued;
    int32_t *forward[2];
    leftover_t *leftovers;
    size_t continued_capacity, forward_capacity[2], leftover_count, leftover_capacity;
    int32_t screen[2];           /* the picture's size in packet coordinates */
};

static int grow(void **items, size_t *capacity, size_t needed, size_t size) {
    size_t n = *capacity ? *capacity : 256;
    void *p;
    if (needed <= *capacity) return 1;
    while (n < needed) n *= 2;
    if (!(p = realloc(*items, n * size))) return 0;
    *items = p; *capacity = n;
    return 1;
}

frame_blend_t *frame_blend_create(void) {
    frame_blend_t *b = calloc(1, sizeof(frame_blend_t));
    if (b) { b->screen[0] = 320; b->screen[1] = 240; }
    return b;
}

void frame_blend_screen(frame_blend_t *b, int32_t width, int32_t height) { b->screen[0] = width; b->screen[1] = height; }

void frame_blend_destroy(frame_blend_t *b) {
    unsigned i;
    if (!b) return;
    for (i = 0; i < 3; ++i) {
        free(b->kept[i].projections); free(b->kept[i].runs); free(b->kept[i].setups); free(b->kept[i].jumped);
        free(b->kept[i].motions); free(b->kept[i].polygons); free(b->kept[i].corner_keys); free(b->kept[i].corner_heads);
        free(b->kept[i].corner_next); free(b->kept[i].colours); free(b->kept[i].packets); free(b->kept[i].pages); free(b->kept[i].coloured);
    }
    free(b->looks); free(b->before); free(b->hidden); free(b->continued); free(b->forward[0]); free(b->forward[1]);
    free(b->leftovers);
    free(b->corners); free(b->codes); free(b->vertices); free(b->keys); free(b->slots); free(b->moves); free(b->surfaces);
    free(b);
}

void frame_blend_reset(frame_blend_t *b) {
    unsigned i;
    for (i = 0; i < 3; ++i) b->kept[i].valid = 0;
    memset(&b->info, 0, sizeof(b->info));
    b->look_count = 0; b->leftover_count = 0;
}

const frame_blend_info_t *frame_blend_info(const frame_blend_t *b) { return &b->info; }

void frame_blend_straight(frame_blend_t *b, int on) { b->straight = on != 0; }

void frame_blend_packet(frame_blend_t *b, uint64_t serial, const uint32_t *packet) {
    unsigned op = packet[0] >> 24, corners = (op & 8) ? 4 : 3, i, k = 1;
    look_t *l;
    if (op < 0x20 || op >= 0x40) return;
    if (b->look_count && b->looks[b->look_count - 1].serial >= serial) b->look_count = 0;
    if (!grow((void **)&b->looks, &b->look_capacity, b->look_count + 1, sizeof(*b->looks))) return;
    l = &b->looks[b->look_count++];
    memset(l, 0, sizeof(*l));
    l->serial = serial; l->opcode = op;
    for (i = 0; i < 12 && i < 1 + corners * (1u + ((op & 4) ? 1u : 0u)) + ((op & 16) ? corners - 1 : 0); ++i) l->words[i] = packet[i];
    l->colours[0] = packet[0] & 0xffffffu;
    for (i = 0; i < corners; ++i) {
        if (i && (op & 16)) l->colours[i] = packet[k++] & 0xffffffu;
        else if (i) l->colours[i] = l->colours[0];
        ++k;
        if (op & 4) {
            if (i == 1) l->page = packet[k] >> 16;
            ++k;
        }
    }
}

static int16_t low(uint32_t w) { return (int16_t)(uint16_t)(w & 0xffffu); }
static int16_t high(uint32_t w) { return (int16_t)(uint16_t)(w >> 16); }

/* The GTE's arithmetic shift and saturation, as gte.c has them. */
static int64_t asr(int64_t n, unsigned bits) {
    return n >= 0 ? (int64_t)((uint64_t)n >> bits) : -1 - (int64_t)((uint64_t)(-1 - n) >> bits);
}
static int64_t saturate(int64_t n, int64_t low, int64_t high) { return n < low ? low : n > high ? high : n; }

uint32_t frame_blend_project(const int32_t view[3], int32_t ofx, int32_t ofy, uint32_t h, uint32_t shift, uint32_t lm) {
    /* RTPS: IR1-2 saturate MAC1-2; SZ3 is MAC3 at a fixed 12-bit shift. */
    int64_t ir1 = saturate(view[0], lm ? 0 : -32768, 32767), ir2 = saturate(view[1], lm ? 0 : -32768, 32767);
    int64_t depth = shift ? view[2] : asr(view[2], 12);
    uint32_t q = gte_divide(h & 65535u, (uint32_t)saturate(depth, 0, 65535));
    int64_t sx = saturate(asr((int64_t)q * ir1 + ofx, 16), -1024, 1023);
    int64_t sy = saturate(asr((int64_t)q * ir2 + ofy, 16), -1024, 1023);
    return ((uint32_t)sx & 65535u) | (uint32_t)sy << 16;
}

static kept_t *kept_at(frame_blend_t *b, unsigned age) { return &b->kept[(b->latest + 3 - age) % 3]; }
static const kept_t *kept_seen(const frame_blend_t *b, unsigned age) { return &b->kept[(b->latest + 3 - age) % 3]; }

static int keep(kept_t *k, const frame_identity_frame_t *f) {
    if (!grow((void **)&k->projections, &k->projection_capacity, f->projection_count, sizeof(*k->projections)) ||
        !grow((void **)&k->runs, &k->run_capacity, f->run_count, sizeof(*k->runs)) ||
        !grow((void **)&k->setups, &k->setup_capacity, f->setup_count, sizeof(*k->setups)) ||
        !grow((void **)&k->jumped, &k->jumped_capacity, f->run_count, sizeof(*k->jumped)) ||
        !grow((void **)&k->polygons, &k->polygon_capacity, f->polygon_count, sizeof(*k->polygons)) ||
        !grow((void **)&k->corner_next, &k->next_capacity, (size_t)f->polygon_count * 4, sizeof(*k->corner_next))) return 0;
    if (!grow((void **)&k->colours, &k->colour_capacity, f->polygon_count, sizeof(*k->colours)) ||
        !grow((void **)&k->pages, &k->page_capacity, f->polygon_count, sizeof(*k->pages)) ||
        !grow((void **)&k->coloured, &k->coloured_capacity, f->polygon_count, sizeof(*k->coloured)) ||
        !grow((void **)&k->packets, &k->packet_capacity, f->polygon_count, sizeof(*k->packets))) return 0;
    if (f->polygon_count) {
        memcpy(k->polygons, f->polygons, f->polygon_count * sizeof(*k->polygons));
        memset(k->coloured, 0, f->polygon_count);
    }
    k->polygon_count = f->polygon_count;
    if (f->run_count) memset(k->jumped, 0, f->run_count);
    if (f->projection_count) memcpy(k->projections, f->projections, f->projection_count * sizeof(*k->projections));
    if (f->run_count) memcpy(k->runs, f->runs, f->run_count * sizeof(*k->runs));
    if (f->setup_count) memcpy(k->setups, f->setups, f->setup_count * sizeof(*k->setups));
    k->projection_count = f->projection_count; k->run_count = f->run_count; k->setup_count = f->setup_count;
    k->number = f->number; k->tag = f->tag; k->valid = 1;
    return 1;
}

/* Newton's method, so that nothing depends on a math library. */
static double root(double x) {
    double r = x > 1 ? x : 1, n;
    unsigned i;
    if (x <= 0) return 0;
    for (i = 0; i < 2000; ++i) {
        n = 0.5 * (r + x / r);
        if (n >= r) break;
        r = n;
    }
    return r;
}

/* The weighted median of n values, reordering them. */
static double median(double *values, double *weights, size_t n) {
    double total = 0, seen = 0;
    size_t i;
    for (i = 0; i < n; ++i) total += weights[i];
    /* By selection: n is a few hundred runs at most. */
    while (n) {
        size_t best = 0;
        for (i = 1; i < n; ++i) if (values[i] < values[best]) best = i;
        seen += weights[best];
        if (seen * 2 >= total) return values[best];
        values[best] = values[n - 1]; weights[best] = weights[n - 1]; --n;
    }
    return 0;
}

/* How a transform run moves between the frame before and its own: its
 * matrix and translation in view units ((TR * 4096 + RT * V) >> sf is
 * t + m * V), at both ends. A run that turns is a rigid part: its vertices
 * are carried by its rotation and translation rather than each moving on a
 * straight line, which would draw a part that turns far shorter between
 * the two frames than at either. */
enum { MOTION_NONE, MOTION_LINE, MOTION_TURN };

/* cos(FRAME_BLEND_TURN_LIMIT / 2) */
#define TURN_COSINE 0.25881904510252074

enum { MOTION_CACHE = 4 };

typedef struct motion {
    uint8_t kind;
    int32_t parent;              /* the run whose transform carries this one's origin, or -1 */
    double m[2][3][3], t[2][3];  /* at both ends */
    double inverse[2][3][3];
    int invertible;
    double j[2][3];              /* with a parent, the origin in the parent's space */
    uint8_t relative;            /* it turns relative to its parent, not on its own */
    uint8_t settled;
    double base[3][3];           /* the rotation at the start, relative to the parent or not */
    double turn[4];              /* from the start to the end, a quaternion with w >= 0 */
    double stretch[2][3][3];     /* what the matrix holds besides its rotation */
    /* Evaluations, by alpha: num, den and the result. */
    uint32_t num[MOTION_CACHE], den[MOTION_CACHE];
    double at_m[MOTION_CACHE][3][3], at_t[MOTION_CACHE][3];
    unsigned next;
} motion_t;

static void matrix_multiply(double a[3][3], double b[3][3], double out[3][3]) {
    double o[3][3];
    unsigned i, j;
    for (i = 0; i < 3; ++i)
        for (j = 0; j < 3; ++j) o[i][j] = a[i][0] * b[0][j] + a[i][1] * b[1][j] + a[i][2] * b[2][j];
    memcpy(out, o, sizeof(o));
}

static void matrix_apply(double m[3][3], const double v[3], double out[3]) {
    double o[3];
    unsigned i;
    for (i = 0; i < 3; ++i) o[i] = m[i][0] * v[0] + m[i][1] * v[1] + m[i][2] * v[2];
    memcpy(out, o, sizeof(o));
}

static double matrix_determinant(double m[3][3]) {
    return m[0][0] * (m[1][1] * m[2][2] - m[1][2] * m[2][1]) - m[0][1] * (m[1][0] * m[2][2] - m[1][2] * m[2][0]) +
           m[0][2] * (m[1][0] * m[2][1] - m[1][1] * m[2][0]);
}

/* The size of a matrix: the root mean square of its singular values. */
static double matrix_size(double m[3][3]) {
    double s = 0;
    unsigned i, j;
    for (i = 0; i < 3; ++i) for (j = 0; j < 3; ++j) s += m[i][j] * m[i][j];
    return root(s / 3);
}

/* The inverse of a matrix that is far from flat: 0 for one that flattens
 * space (a shadow squashed onto the floor, a part scaled to nothing), whose
 * rotation means nothing. */
static int matrix_inverse(double m[3][3], double out[3][3]) {
    double d = matrix_determinant(m), s = matrix_size(m);
    unsigned i, j;
    if (s <= 0 || !(d * d >= s * s * s * s * s * s * 0.01)) return 0;
    for (i = 0; i < 3; ++i)
        for (j = 0; j < 3; ++j) {
            unsigned a = (j + 1) % 3, b = (j + 2) % 3, c = (i + 1) % 3, e = (i + 2) % 3;
            out[i][j] = (m[a][c] * m[b][e] - m[a][e] * m[b][c]) / d;
        }
    return 1;
}

/* M = Q P, Q a proper rotation and P what remains (scale, shear and, for a
 * mirrored M, the mirror), by Newton's iteration Q <- (Q + Q^-T) / 2, which
 * converges to the orthogonal factor of the polar decomposition. */
static int polar(double m[3][3], double q[3][3], double p[3][3]) {
    double x[3][3], y[3][3], s = matrix_size(m);
    unsigned i, j, k;
    if (s <= 0) return 0;
    for (i = 0; i < 3; ++i) for (j = 0; j < 3; ++j) x[i][j] = m[i][j] / s;
    for (k = 0; k < 64; ++k) {
        double change = 0;
        if (!matrix_inverse(x, y)) return 0;
        for (i = 0; i < 3; ++i)
            for (j = 0; j < 3; ++j) {
                double n = (x[i][j] + y[j][i]) / 2;
                change += (n - x[i][j]) * (n - x[i][j]);
                x[i][j] = n;
            }
        if (change < 1e-24) break;
    }
    /* A mirror is no rotation: keep it in P, the same at both ends or no turn. */
    if (matrix_determinant(x) < 0) for (i = 0; i < 3; ++i) for (j = 0; j < 3; ++j) x[i][j] = -x[i][j];
    memcpy(q, x, sizeof(x));
    for (i = 0; i < 3; ++i)
        for (j = 0; j < 3; ++j) p[i][j] = x[0][i] * m[0][j] + x[1][i] * m[1][j] + x[2][i] * m[2][j];
    return 1;
}

/* A rotation matrix as a unit quaternion w, x, y, z. */
static void quaternion(double m[3][3], double q[4]) {
    double t = m[0][0] + m[1][1] + m[2][2], s, n;
    unsigned i;
    if (t > 0) {
        s = root(t + 1) * 2;
        q[0] = s / 4; q[1] = (m[2][1] - m[1][2]) / s; q[2] = (m[0][2] - m[2][0]) / s; q[3] = (m[1][0] - m[0][1]) / s;
    } else if (m[0][0] > m[1][1] && m[0][0] > m[2][2]) {
        s = root(1 + m[0][0] - m[1][1] - m[2][2]) * 2;
        q[0] = (m[2][1] - m[1][2]) / s; q[1] = s / 4; q[2] = (m[0][1] + m[1][0]) / s; q[3] = (m[0][2] + m[2][0]) / s;
    } else if (m[1][1] > m[2][2]) {
        s = root(1 + m[1][1] - m[0][0] - m[2][2]) * 2;
        q[0] = (m[0][2] - m[2][0]) / s; q[1] = (m[0][1] + m[1][0]) / s; q[2] = s / 4; q[3] = (m[1][2] + m[2][1]) / s;
    } else {
        s = root(1 + m[2][2] - m[0][0] - m[1][1]) * 2;
        q[0] = (m[1][0] - m[0][1]) / s; q[1] = (m[0][2] + m[2][0]) / s; q[2] = (m[1][2] + m[2][1]) / s; q[3] = s / 4;
    }
    n = root(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
    /* q and -q are one rotation; w >= 0 is the shorter way to it. */
    if (q[0] < 0) n = -n;
    for (i = 0; i < 4; ++i) q[i] /= n;
}

static void rotation(const double q[4], double m[3][3]) {
    double w = q[0], x = q[1], y = q[2], z = q[3];
    m[0][0] = 1 - 2 * (y * y + z * z); m[0][1] = 2 * (x * y - w * z); m[0][2] = 2 * (x * z + w * y);
    m[1][0] = 2 * (x * y + w * z); m[1][1] = 1 - 2 * (x * x + z * z); m[1][2] = 2 * (y * z - w * x);
    m[2][0] = 2 * (x * z - w * y); m[2][1] = 2 * (y * z + w * x); m[2][2] = 1 - 2 * (x * x + y * y);
}

static void normalize4(double q[4]) {
    double n = root(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
    unsigned i;
    if (n > 0) for (i = 0; i < 4; ++i) q[i] /= n;
}

/* The rotation a fraction a of the way along the arc from none to q (w >= 0),
 * at constant speed: the normalized sum of two unit quaternions lies halfway
 * between them on their arc, so halving the arc finds any fraction of it with
 * nothing but square roots. */
static void turn_by(const double q[4], double a, double out[4]) {
    double lo[4] = {1, 0, 0, 0}, hi[4], from = 0, to = 1, f;
    unsigned i, k;
    memcpy(hi, q, sizeof(hi));
    for (k = 0; k < 20; ++k) {
        double mid[4], at = (from + to) / 2;
        for (i = 0; i < 4; ++i) mid[i] = lo[i] + hi[i];
        normalize4(mid);
        if (a < at) { memcpy(hi, mid, sizeof(mid)); to = at; }
        else { memcpy(lo, mid, sizeof(mid)); from = at; }
    }
    f = to > from ? (a - from) / (to - from) : 0;
    for (i = 0; i < 4; ++i) out[i] = lo[i] + (hi[i] - lo[i]) * f;
    normalize4(out);
}

/* A setup's matrix and translation in view units. */
static void setup_transform(const frame_identity_setup_t *s, double m[3][3], double t[3]) {
    double unit = s->shift ? 1.0 / 4096 : 1;
    int16_t r[9];
    unsigned i;
    for (i = 0; i < 9; ++i) r[i] = (int16_t)(uint16_t)(s->rotation[i / 2] >> (i % 2 * 16));
    for (i = 0; i < 9; ++i) m[i / 3][i % 3] = r[i] * unit;
    for (i = 0; i < 3; ++i) t[i] = (double)s->translation[i] * 4096 * unit;
}

static double distance(const double a[3], const double b[3]) {
    return root((a[0] - b[0]) * (a[0] - b[0]) + (a[1] - b[1]) * (a[1] - b[1]) + (a[2] - b[2]) * (a[2] - b[2]));
}

/* Whether a's transform carries b's origin from one end to the other: b's
 * origin, put in a's space at one end, is where b's origin is at the other,
 * both ways, to within what the GTE's fixed point allows over that distance.
 * Then b hangs from a, as a bone from its parent, for this interval. */
static int carries(struct motion *a, struct motion *b, double *span) {
    double d[3], j[3], at[3];
    unsigned e, i;
    *span = distance(b->t[0], a->t[0]);
    for (e = 0; e < 2; ++e) {
        for (i = 0; i < 3; ++i) d[i] = b->t[e][i] - a->t[e][i];
        matrix_apply(a->inverse[e], d, j);
        matrix_apply(a->m[e ^ 1], j, at);
        for (i = 0; i < 3; ++i) at[i] += a->t[e ^ 1][i];
        if (distance(at, b->t[e ^ 1]) > 2 + *span / 100) return 0;
    }
    return 1;
}

/* How run i turns, once its parent's motion is known. Relative to its
 * parent, when it has one that turns, or on its own, whichever is the
 * smaller turn: the parts of a chain bend little at each joint, but a part
 * may also spin in place against what carries it (a weapon flipped in a
 * hand). A parent that moves on straight lines, whose matrix between the
 * ends is no rotation, carries nothing. */
static void settle(frame_blend_t *b, kept_t *cur, size_t i, unsigned depth) {
    motion_t *mo = &cur->motions[i];
    double best[4] = {-2, 0, 0, 0}, base[3][3], stretch[2][3][3];
    unsigned variant, e, x, y;
    if (mo->settled) return;
    mo->settled = 1;
    if (mo->kind != MOTION_TURN) return;
    if (mo->parent >= 0) {
        if (depth < 64) settle(b, cur, (size_t)mo->parent, depth + 1);
        if (depth >= 64 || cur->motions[mo->parent].kind != MOTION_TURN) mo->parent = -1;
    }
    mo->relative = 0;
    for (variant = 0; variant < (mo->parent >= 0 ? 2u : 1u); ++variant) {
        double local[2][3][3], rot[2][3][3], p[2][3][3], delta[3][3], turn[4];
        for (e = 0; e < 2; ++e) {
            if (variant) matrix_multiply(cur->motions[mo->parent].inverse[e], mo->m[e], local[e]);
            else memcpy(local[e], mo->m[e], sizeof(local[e]));
            if (!polar(local[e], rot[e], p[e])) break;
        }
        /* A mirror at one end and not the other is no turn. */
        if (e < 2 || (matrix_determinant(local[0]) < 0) != (matrix_determinant(local[1]) < 0)) continue;
        for (x = 0; x < 3; ++x)
            for (y = 0; y < 3; ++y) delta[x][y] = rot[0][0][x] * rot[1][0][y] + rot[0][1][x] * rot[1][1][y] + rot[0][2][x] * rot[1][2][y];
        quaternion(delta, turn);
        if (turn[0] <= best[0]) continue;
        memcpy(best, turn, sizeof(best)); memcpy(base, rot[0], sizeof(base)); memcpy(stretch, p, sizeof(stretch));
        mo->relative = (uint8_t)variant;
    }
    if (best[0] < -1) {
        ++b->info.runs_flat;
        mo->kind = MOTION_LINE; mo->parent = -1;
        return;
    }
    /* w is the cosine of half the angle. Past the limit, which way the part
     * went is a guess: see FRAME_BLEND_TURN_LIMIT. */
    if (best[0] < TURN_COSINE) {
        ++b->info.runs_far;
        mo->kind = MOTION_LINE; mo->parent = -1;
        return;
    }
    memcpy(mo->turn, best, sizeof(mo->turn)); memcpy(mo->base, base, sizeof(mo->base));
    memcpy(mo->stretch, stretch, sizeof(mo->stretch));
    if (mo->parent >= 0)
        for (e = 0; e < 2; ++e) {
            motion_t *a = &cur->motions[mo->parent];
            double d[3];
            for (x = 0; x < 3; ++x) d[x] = mo->t[e][x] - a->t[e][x];
            matrix_apply(a->inverse[e], d, mo->j[e]);
        }
}

/* How each run of cur that pairs with prev moves. A run whose matrix is a
 * rotation times anything that stays (scale, shear) turns: its rotation is
 * interpolated along the shorter arc and the rest linearly. A run hanging
 * from another (carries) turns relative to it, as a bone relative to its
 * parent, so a chain of parts that each swing far against the camera (a
 * whip, a limb) bends at each joint by a smaller angle and its joints stay
 * where both parts put them. Everything else moves on straight lines. */
static int motion_plan(frame_blend_t *b, kept_t *cur, const kept_t *prev) {
    size_t i, k;
    if (!grow((void **)&cur->motions, &cur->motion_capacity, cur->run_count, sizeof(*cur->motions))) return 0;
    for (i = 0; i < cur->run_count; ++i) {
        motion_t *mo = &cur->motions[i];
        const frame_identity_run_t *r = &cur->runs[i], *q;
        const frame_identity_setup_t *s1, *s0;
        mo->kind = MOTION_NONE; mo->parent = -1; mo->invertible = 0; mo->next = 0; mo->settled = 0;
        for (k = 0; k < MOTION_CACHE; ++k) mo->den[k] = 0;
        if (b->straight || cur->reason != FRAME_BLEND_KEPT || r->previous < 0 || (size_t)r->previous >= prev->run_count)
            continue;
        q = &prev->runs[r->previous];
        if (q->count != r->count || (size_t)q->first + q->count > prev->projection_count ||
            (size_t)r->first + r->count > cur->projection_count) continue;
        s1 = &cur->setups[r->setup]; s0 = &prev->setups[q->setup];
        mo->kind = MOTION_LINE;
        if (s1->shift != s0->shift) continue;
        /* The same vertices at both ends, or nothing rigid to follow. */
        for (k = 0; k < r->count; ++k)
            if (memcmp(cur->projections[r->first + k].input, prev->projections[q->first + k].input, sizeof(int16_t) * 3)) break;
        if (k < r->count) continue;
        setup_transform(s0, mo->m[0], mo->t[0]); setup_transform(s1, mo->m[1], mo->t[1]);
        mo->invertible = matrix_inverse(mo->m[0], mo->inverse[0]) && matrix_inverse(mo->m[1], mo->inverse[1]);
        if (mo->invertible) mo->kind = MOTION_TURN;
        else ++b->info.runs_flat;
    }
    /* Parents: the nearest run that carries this one's origin. */
    for (i = 0; i < cur->run_count; ++i) {
        motion_t *mo = &cur->motions[i];
        double best = 0, span;
        if (mo->kind != MOTION_TURN) continue;
        for (k = 0; k < cur->run_count; ++k) {
            motion_t *a = &cur->motions[k];
            if (k == i || a->kind != MOTION_TURN || cur->setups[cur->runs[k].setup].shift != cur->setups[cur->runs[i].setup].shift)
                continue;
            if (carries(a, mo, &span) && (mo->parent < 0 || span < best)) { mo->parent = (int32_t)k; best = span; }
        }
    }
    /* Runs that move together carry each other: break each loop once. */
    for (i = 0; i < cur->run_count; ++i) {
        int32_t at = cur->motions[i].parent;
        unsigned depth = 0;
        while (at >= 0 && (size_t)at != i && depth++ < 64) at = cur->motions[at].parent;
        if (at >= 0) cur->motions[i].parent = -1;
    }
    for (i = 0; i < cur->run_count; ++i) {
        settle(b, cur, i, 0);
        if (cur->motions[i].kind == MOTION_TURN) {
            ++b->info.runs_turning;
            if (cur->motions[i].parent >= 0) ++b->info.runs_attached;
        }
    }
    return 1;
}

static int64_t nearest(double x) { return x >= 0 ? (int64_t)(x + 0.5) : -(int64_t)(-x + 0.5); }

/* A run's matrix and translation at alpha = num / den. */
static void motion_at(motion_t *motions, uint32_t run, uint32_t num, uint32_t den, double m[3][3], double t[3], unsigned depth) {
    motion_t *mo = &motions[run];
    double a = (double)num / den;
    unsigned k, x, y;
    for (k = 0; k < MOTION_CACHE; ++k)
        if (mo->den[k] == den && mo->num[k] == num) {
            memcpy(m, mo->at_m[k], sizeof(mo->at_m[k])); memcpy(t, mo->at_t[k], sizeof(mo->at_t[k]));
            return;
        }
    if (mo->kind != MOTION_TURN || depth > 64) {
        for (x = 0; x < 3; ++x) {
            for (y = 0; y < 3; ++y) m[x][y] = mo->m[0][x][y] + (mo->m[1][x][y] - mo->m[0][x][y]) * a;
            t[x] = mo->t[0][x] + (mo->t[1][x] - mo->t[0][x]) * a;
        }
    } else {
        double q[4], r[3][3], s[3][3], local[3][3];
        turn_by(mo->turn, a, q);
        rotation(q, r);
        matrix_multiply(mo->base, r, r);
        for (x = 0; x < 3; ++x)
            for (y = 0; y < 3; ++y) s[x][y] = mo->stretch[0][x][y] + (mo->stretch[1][x][y] - mo->stretch[0][x][y]) * a;
        matrix_multiply(r, s, local);
        if (mo->parent >= 0) {
            double pm[3][3], pt[3], j[3];
            motion_at(motions, (uint32_t)mo->parent, num, den, pm, pt, depth + 1);
            for (x = 0; x < 3; ++x) j[x] = mo->j[0][x] + (mo->j[1][x] - mo->j[0][x]) * a;
            if (mo->relative) matrix_multiply(pm, local, m);
            else memcpy(m, local, sizeof(local));
            matrix_apply(pm, j, t);
            for (x = 0; x < 3; ++x) t[x] += pt[x];
        } else {
            memcpy(m, local, sizeof(local));
            for (x = 0; x < 3; ++x) t[x] = mo->t[0][x] + (mo->t[1][x] - mo->t[0][x]) * a;
        }
    }
    k = mo->next++ % MOTION_CACHE;
    mo->num[k] = num; mo->den[k] = den;
    memcpy(mo->at_m[k], m, sizeof(mo->at_m[k])); memcpy(mo->at_t[k], t, sizeof(mo->at_t[k]));
}

/* Where a vertex of a turning run stands at alpha, in view space. */
static void turned(motion_t *motions, const vertex_t *v, uint32_t num, uint32_t den, int32_t view[3]) {
    double m[3][3], t[3], in[3] = {v->input[0], v->input[1], v->input[2]}, out[3];
    unsigned x;
    motion_at(motions, (uint32_t)v->run, num, den, m, t, 0);
    matrix_apply(m, in, out);
    for (x = 0; x < 3; ++x) {
        int64_t n = nearest(out[x] + t[x]);
        view[x] = (int32_t)(n > INT32_MAX ? INT32_MAX : n < INT32_MIN ? INT32_MIN : n);
    }
}

/* The cut measures, over each run paired with the frame before: how far its
 * centre moved in view space over its depth (move), and how far it stands
 * from where it would be had it kept the velocity it had one frame earlier
 * (change; the whole move when that is unknown). Vertex-weighted medians,
 * in thousandths. A camera that swoops close past a model moves its runs by
 * several times their depth each frame, but steadily; a cut moves almost
 * every run by a large part of its depth at once, and nothing before it
 * predicts that. */
static int cut_measure(frame_blend_t *b, kept_t *cur, const kept_t *prev, const kept_t *older,
                       uint32_t *move, uint32_t *change) {
    size_t i, n = 0, size = cur->run_count * 4;
    /* The frame before needs pairs of its own, cut or not: after a cut the
     * velocity is the jump, which predicts nothing, and the frame is held if
     * it moves far again. */
    int velocity = older->valid && older->number + 1 == prev->number &&
                   (prev->reason == FRAME_BLEND_KEPT || prev->reason == FRAME_BLEND_CUT);
    double *moves, *changes, *weights, *weights2;
    *move = *change = 0;
    if (!grow((void **)&b->moves, &b->move_capacity, size, sizeof(*b->moves))) return 0;
    moves = b->moves; changes = moves + cur->run_count;
    weights = changes + cur->run_count; weights2 = weights + cur->run_count;
    for (i = 0; i < cur->run_count; ++i) {
        const frame_identity_run_t *r = &cur->runs[i], *q, *o = NULL;
        double c[3] = {0, 0, 0}, d[3] = {0, 0, 0}, e[3] = {0, 0, 0}, z, dd = 0, ee = 0;
        uint32_t k, a;
        if (r->previous < 0 || (size_t)r->previous >= prev->run_count) continue;
        q = &prev->runs[r->previous];
        if (q->count != r->count || (size_t)q->first + q->count > prev->projection_count) continue;
        if (velocity && q->previous >= 0 && (size_t)q->previous < older->run_count) {
            o = &older->runs[q->previous];
            if (o->count != r->count || (size_t)o->first + o->count > older->projection_count) o = NULL;
        }
        for (k = 0; k < r->count; ++k)
            for (a = 0; a < 3; ++a) {
                c[a] += cur->projections[r->first + k].view[a];
                d[a] += prev->projections[q->first + k].view[a];
                if (o) e[a] += older->projections[o->first + k].view[a];
            }
        for (a = 0; a < 3; ++a) {
            c[a] /= r->count; d[a] /= r->count; e[a] /= r->count;
            dd += (c[a] - d[a]) * (c[a] - d[a]);
            if (o) ee += (2 * d[a] - e[a] - c[a]) * (2 * d[a] - e[a] - c[a]);
        }
        z = c[2] > d[2] ? c[2] : d[2];
        if (z < 64) z = 64;
        moves[n] = root(dd) / z; changes[n] = o ? root(ee) / z : moves[n]; weights[n] = weights2[n] = r->count;
        /* The same rule for the run alone: a model that is somewhere else at
         * once (an attacker put in front of the camera) while the scene
         * around it moves on. */
        cur->jumped[i] = moves[n] * 1000 >= FRAME_BLEND_CUT_MOVE && changes[n] * 2 >= moves[n];
        ++n;
    }
    if (!n) return 1;
    *move = (uint32_t)(median(moves, weights, n) * 1000 + 0.5);
    *change = (uint32_t)(median(changes, weights2, n) * 1000 + 0.5);
    return 1;
}

static uint32_t screen_move(frame_blend_t *b, const kept_t *cur, const kept_t *prev) {
    uint64_t n = 0, seen = 0;
    size_t i;
    uint32_t d;
    memset(b->histogram, 0, sizeof(b->histogram));
    for (i = 0; i < cur->projection_count; ++i) {
        int32_t p = cur->projections[i].previous;
        uint32_t x, y;
        if (p < 0 || (size_t)p >= prev->projection_count) continue;
        x = (uint32_t)abs(low(cur->projections[i].sxy) - low(prev->projections[p].sxy));
        y = (uint32_t)abs(high(cur->projections[i].sxy) - high(prev->projections[p].sxy));
        d = x > y ? x : y;
        ++b->histogram[d < 2047 ? d : 2047];
        ++n;
    }
    for (d = 0; d < 2048 && n; ++d) if ((seen += b->histogram[d]) * 2 >= n) return d;
    return 0;
}

static size_t table_slot(const frame_blend_t *b, uint64_t key) {
    size_t i = (size_t)(((key * UINT64_C(0x9e3779b97f4a7c15)) >> 32) & (b->table_capacity - 1));
    while (b->slots[i] >= 0 && b->keys[i] != key) i = (i + 1) & (b->table_capacity - 1);
    return i;
}

static int table_reset(frame_blend_t *b, size_t entries) {
    size_t n = 64;
    while (n < entries * 2) n *= 2;
    if (n > b->table_capacity) {
        uint64_t *keys = realloc(b->keys, n * sizeof(*keys));
        int32_t *slots;
        if (!keys) return 0;
        b->keys = keys;
        if (!(slots = realloc(b->slots, n * sizeof(*slots)))) return 0;
        b->slots = slots; b->table_capacity = n;
    }
    memset(b->slots, 0xff, b->table_capacity * sizeof(*b->slots));
    return 1;
}

/* The vertex at a packet position of a surface, made if there is none. */
static int32_t vertex_at(frame_blend_t *b, uint32_t xy, uint32_t surface) {
    uint64_t key = xy | (uint64_t)surface << 32;
    size_t i = table_slot(b, key);
    if (b->slots[i] >= 0) return b->slots[i];
    if (!grow((void **)&b->vertices, &b->vertex_capacity, b->vertex_count + 1, sizeof(*b->vertices))) return -1;
    memset(&b->vertices[b->vertex_count], 0, sizeof(*b->vertices));
    b->vertices[b->vertex_count].pinned = -1; /* not yet given ends */
    b->keys[i] = key; b->slots[i] = (int32_t)b->vertex_count;
    return (int32_t)b->vertex_count++;
}


static uint32_t surface_of(int32_t *surfaces, uint32_t i) {
    uint32_t root = i, next;
    while ((uint32_t)surfaces[root] != root) root = (uint32_t)surfaces[root];
    while ((uint32_t)surfaces[i] != root) { next = (uint32_t)surfaces[i]; surfaces[i] = (int32_t)root; i = next; }
    return root;
}

static int is_3d(const frame_identity_polygon_t *q) {
    unsigned c;
    for (c = 0; c < q->corners; ++c) if (q->state[c] != FRAME_IDENTITY_NO_PROJECTION) return 1;
    return 0;
}

/* 3D polygons that share an edge, both its ends at the same packet positions,
 * are one surface. Only along an edge can a moving polygon open a gap against
 * a held one; two surfaces that meet at a single position, often by chance
 * (an effect's corner on a vertex of the floor), do not hold each other. */
static int surfaces(frame_blend_t *b, const frame_identity_frame_t *f) {
    static const unsigned triangle[3][2] = {{0, 1}, {1, 2}, {2, 0}}, quad[4][2] = {{0, 1}, {1, 3}, {3, 2}, {2, 0}};
    uint32_t i, e;
    if (!grow((void **)&b->surfaces, &b->surface_capacity, f->polygon_count, sizeof(*b->surfaces)) ||
        !table_reset(b, (size_t)f->polygon_count * 4)) return 0;
    for (i = 0; i < f->polygon_count; ++i) b->surfaces[i] = (int32_t)i;
    for (i = 0; i < f->polygon_count; ++i) {
        const frame_identity_polygon_t *q = &f->polygons[i];
        if (!is_3d(q)) continue;
        for (e = 0; e < q->corners; ++e) {
            uint32_t p = q->xy[q->corners == 4 ? quad[e][0] : triangle[e][0]], r = q->xy[q->corners == 4 ? quad[e][1] : triangle[e][1]];
            uint64_t key = p < r ? (uint64_t)p << 32 | r : (uint64_t)r << 32 | p;
            size_t slot;
            if (p == r) continue;
            slot = table_slot(b, key);
            if (b->slots[slot] < 0) { b->keys[slot] = key; b->slots[slot] = (int32_t)i; }
            else {
                uint32_t x = surface_of(b->surfaces, i), y = surface_of(b->surfaces, (uint32_t)b->slots[slot]);
                if (x != y) b->surfaces[x > y ? x : y] = (int32_t)(x < y ? x : y);
            }
        }
    }
    return table_reset(b, (size_t)f->polygon_count * 4);
}

static int near_plane(const int32_t view[3], const frame_identity_setup_t *s) {
    int64_t depth = saturate(s->shift ? view[2] : asr(view[2], 12), 0, 65535);
    return (int64_t)(s->h & 65535u) >= depth * 2;
}

/* Why a 3D corner cannot move by itself. */
enum { ENDS_OK, ENDS_SERVICE, ENDS_PROJECTION, ENDS_RESET, ENDS_JUMP };

/* The two ends of a linked corner, or why there are none to trust. */
static unsigned ends(frame_blend_t *b, const frame_identity_polygon_t *q, unsigned c, vertex_t *v) {
    const kept_t *at, *before;
    const frame_identity_projection_t *p, *o;
    const frame_identity_setup_t *sp, *so;
    if (q->state[c] == FRAME_IDENTITY_NO_HISTORY || q->state[c] == FRAME_IDENTITY_EXCLUDED_CORNER) return ENDS_RESET;
    if (q->state[c] != FRAME_IDENTITY_LINKED) return ENDS_SERVICE;
    if (q->age[c] > 1) return ENDS_RESET;
    at = kept_at(b, q->age[c]); before = kept_at(b, q->age[c] + 1u);
    if (!at->valid || at->reason != FRAME_BLEND_KEPT || !before->valid || at->number != before->number + 1)
        return ENDS_RESET;
    if (q->projection[c] < 0 || (size_t)q->projection[c] >= at->projection_count) return ENDS_PROJECTION;
    p = &at->projections[q->projection[c]];
    if (p->previous < 0 || (size_t)p->previous >= before->projection_count) return ENDS_PROJECTION;
    if (p->run < at->run_count && at->jumped[p->run]) return ENDS_JUMP;
    o = &before->projections[p->previous];
    if (p->run >= at->run_count || o->run >= before->run_count) return ENDS_PROJECTION;
    sp = &at->setups[at->runs[p->run].setup]; so = &before->setups[before->runs[o->run].setup];
    if (sp->shift != so->shift || sp->lm != so->lm) return ENDS_PROJECTION;
    /* At or behind the camera plane the GTE's division saturates and its
     * projection is no longer a perspective one: nothing between such an end
     * and the other is a place the vertex passed. */
    if (near_plane(p->view, sp) || near_plane(o->view, so)) return ENDS_PROJECTION;
    /* Both ends must be what the GTE's equations give for them: a MAC that
     * wrapped, or a packet word other than the projection's, is not followed. */
    if (q->xy[c] != p->sxy || frame_blend_project(p->view, sp->ofx, sp->ofy, sp->h, sp->shift, sp->lm) != p->sxy ||
        frame_blend_project(o->view, so->ofx, so->ofy, so->h, so->shift, so->lm) != o->sxy) return ENDS_PROJECTION;
    memcpy(v->view[0], o->view, sizeof(o->view)); memcpy(v->view[1], p->view, sizeof(p->view));
    v->ofx[0] = so->ofx; v->ofy[0] = so->ofy; v->h[0] = so->h; v->sxy[0] = o->sxy;
    v->ofx[1] = sp->ofx; v->ofy[1] = sp->ofy; v->h[1] = sp->h; v->sxy[1] = p->sxy;
    v->shift = sp->shift; v->lm = sp->lm;
    v->pinned = 0;
    v->run = -1;
    if (p->run < at->run_count && at->motions[p->run].kind == MOTION_TURN) {
        uint32_t k;
        v->run = (int32_t)p->run; v->age = q->age[c];
        memcpy(v->input, p->input, sizeof(v->input));
        /* A straight line between two ends in front of the camera stays in
         * front; an arc may not. */
        for (k = 1; k < 4 && v->run >= 0; ++k) {
            int32_t view[3];
            int64_t h = (int64_t)(v->h[0] & 65535u) + ((int64_t)(v->h[1] & 65535u) - (int64_t)(v->h[0] & 65535u)) * k / 4;
            turned(at->motions, v, k, 4, view);
            if (h >= saturate(v->shift ? view[2] : asr(view[2], 12), 0, 65535) * 2) v->run = -1;
        }
    }
    return ENDS_OK;
}

/* The index of a kept frame's polygons by linked corner. */
static uint64_t corner_key(unsigned c, unsigned age, int32_t projection) {
    return (uint64_t)c << 40 | (uint64_t)age << 32 | (uint32_t)projection;
}

static size_t corner_slot(const kept_t *k, uint64_t key) {
    size_t i = (size_t)(((key * UINT64_C(0x9e3779b97f4a7c15)) >> 32) & (k->corner_capacity - 1));
    while (k->corner_heads[i] >= 0 && k->corner_keys[i] != key) i = (i + 1) & (k->corner_capacity - 1);
    return i;
}

static int index_corners(kept_t *k) {
    size_t n = 64, i;
    unsigned c;
    while (n < k->polygon_count * 8) n *= 2;
    if (n > k->corner_capacity) {
        uint64_t *keys = realloc(k->corner_keys, n * sizeof(*keys));
        int32_t *heads;
        if (!keys) return 0;
        k->corner_keys = keys;
        if (!(heads = realloc(k->corner_heads, n * sizeof(*heads)))) return 0;
        k->corner_heads = heads; k->corner_capacity = n;
    }
    memset(k->corner_heads, 0xff, k->corner_capacity * sizeof(*k->corner_heads));
    /* Backwards, so that each chain runs in drawing order. */
    for (i = k->polygon_count; i-- > 0;)
        for (c = 0; c < k->polygons[i].corners; ++c) {
            const frame_identity_polygon_t *q = &k->polygons[i];
            uint64_t key;
            size_t slot;
            k->corner_next[i * 4 + c] = -1;
            if (q->state[c] != FRAME_IDENTITY_LINKED) continue;
            key = corner_key(c, q->age[c], q->projection[c]);
            slot = corner_slot(k, key);
            k->corner_next[i * 4 + c] = k->corner_heads[slot];
            k->corner_keys[slot] = key; k->corner_heads[slot] = (int32_t)i;
        }
    return 1;
}

/* Where a linked corner's projection stood a game frame earlier, by index in
 * that frame's projections, or -1. */
static int32_t pair_of(frame_blend_t *b, const frame_identity_polygon_t *q, unsigned c) {
    const kept_t *at;
    if (q->state[c] != FRAME_IDENTITY_LINKED || q->age[c] > 1) return -1;
    at = kept_at(b, q->age[c]);
    if (!at->valid || q->projection[c] < 0 || (size_t)q->projection[c] >= at->projection_count) return -1;
    return at->projections[q->projection[c]].previous;
}

/* The polygon the game drew a game frame earlier as this one: in the frame
 * before, with the same command, whose linked corners are, corner by corner,
 * the pairs of this one's. The game draws a model's polygons from the same
 * list every frame, so that is the same polygon. -1 if there is none, or if
 * several that differ where this one's corners are not linked. */
static int32_t predecessor(frame_blend_t *b, const frame_identity_polygon_t *q) {
    const kept_t *cur = kept_at(b, 0), *prev = kept_at(b, 1);
    int32_t found = -1, at = -1, pair;
    unsigned c, first = 4;
    if (!prev->valid || prev->number + 1 != cur->number || !prev->corner_capacity) return -1;
    for (c = 0; c < q->corners && first == 4; ++c) if (pair_of(b, q, c) >= 0) first = c;
    if (first == 4) return -1;
    pair = pair_of(b, q, first);
    at = prev->corner_heads[corner_slot(prev, corner_key(first, q->age[first], pair))];
    for (; at >= 0; at = prev->corner_next[(size_t)at * 4 + first]) {
        const frame_identity_polygon_t *r = &prev->polygons[at];
        unsigned same = r->opcode == q->opcode && r->corners == q->corners;
        for (c = 0; c < q->corners && same; ++c) {
            if (q->state[c] != FRAME_IDENTITY_LINKED) continue;
            same = r->state[c] == FRAME_IDENTITY_LINKED && r->age[c] == q->age[c] && r->projection[c] == pair_of(b, q, c);
        }
        if (!same) continue;
        if (found >= 0) {
            for (c = 0; c < q->corners; ++c) if (prev->polygons[found].xy[c] != r->xy[c]) return -1;
            continue;
        }
        found = at;
    }
    return found;
}

/* Corners the service held as ambiguous, several projections at their
 * position that stood apart before (parts that cross on screen, a coiled
 * chain drawn small), resolved by the same polygon a game frame earlier: its
 * packet word there is where the vertex stood, so the projection at this
 * position whose pair stood exactly there is the vertex. Several that did,
 * all stood together then and now, and are one vertex. */
static void resolve(frame_blend_t *b, kept_t *cur) {
    size_t i, k;
    unsigned c, n;
    for (i = 0; i < cur->polygon_count; ++i) {
        frame_identity_polygon_t *q = &cur->polygons[i];
        const frame_identity_polygon_t *r;
        const kept_t *at, *before;
        int32_t from;
        unsigned age = 2, ambiguous = 0;
        for (c = 0; c < q->corners; ++c) {
            if (q->state[c] == FRAME_IDENTITY_AMBIGUOUS) ++ambiguous;
            else if (q->state[c] == FRAME_IDENTITY_LINKED) age = q->age[c];
        }
        if (!ambiguous || age > 1 || (from = predecessor(b, q)) < 0) continue;
        r = &kept_at(b, 1)->polygons[from];
        at = kept_at(b, age); before = kept_at(b, age + 1);
        if (!at->valid || !before->valid || at->number != before->number + 1) continue;
        for (c = 0; c < q->corners; ++c) {
            int32_t chosen = -1;
            if (q->state[c] != FRAME_IDENTITY_AMBIGUOUS) continue;
            for (k = 0, n = 0; k < at->projection_count; ++k) {
                const frame_identity_projection_t *p = &at->projections[k];
                unsigned d;
                if (p->sxy != q->xy[c] || p->previous < 0 || (size_t)p->previous >= before->projection_count ||
                    before->projections[p->previous].sxy != r->xy[c]) continue;
                ++n;
                /* Prefer the run of the polygon's other corners. */
                for (d = 0; d < q->corners; ++d)
                    if (q->state[d] == FRAME_IDENTITY_LINKED && at->projections[q->projection[d]].run == p->run) chosen = (int32_t)k;
                if (chosen < 0 && n == 1) chosen = (int32_t)k;
            }
            if (chosen < 0) continue;
            q->state[c] = FRAME_IDENTITY_LINKED; q->age[c] = (uint8_t)age; q->projection[c] = chosen;
            ++b->info.corners_resolved;
        }
    }
}

/* The packets of a frame's polygons, by serial; older ones are dropped. */
static void attach_looks(frame_blend_t *b, kept_t *k, const frame_identity_frame_t *f) {
    size_t i, kept = 0;
    for (i = 0; i < b->look_count; ++i) {
        const look_t *l = &b->looks[i];
        uint64_t at = l->serial - f->first_polygon;
        if (l->serial >= f->first_polygon && at < f->polygon_count) {
            if (f->polygons[at].opcode != l->opcode) continue;
            memcpy(k->colours[at], l->colours, sizeof(l->colours));
            memcpy(k->packets[at], l->words, sizeof(l->words));
            k->pages[at] = l->page; k->coloured[at] = 1;
        } else if (l->serial >= f->first_polygon + f->polygon_count) b->looks[kept++] = *l;
    }
    b->look_count = kept;
}

/* Whether the GPU draws any of a polygon: it skips a triangle whose corners
 * lie 1024 or more apart across or 512 or more down, as polygons with a
 * corner far off screen, where the GTE clamps it, are. */
static int drawn(const frame_identity_polygon_t *q) {
    unsigned t, k;
    for (t = 0; t + 3 <= q->corners; ++t) {
        int l = 1 << 20, r = -(1 << 20), top = 1 << 20, bottom = -(1 << 20);
        for (k = t; k < t + 3; ++k) {
            int x = (int)(q->xy[k] & 1023u) - (int)(q->xy[k] & 1024u), y = (int)((q->xy[k] >> 16) & 1023u) - (int)((q->xy[k] >> 16) & 1024u);
            l = x < l ? x : l; r = x > r ? x : r; top = y < top ? y : top; bottom = y > bottom ? y : bottom;
        }
        if (r - l < 1024 && bottom - top < 512) return 1;
    }
    return 0;
}

/* Polygons of the frame before that the latest frame no longer draws because
 * they left the screen: on a fast camera move, what the older picture had at
 * the edge, which the newer one culls. Between the two they are drawn first,
 * beneath everything, moved by their vertices' own motion, which their
 * projections a game frame later give: the game projects a model's vertices
 * before it culls its polygons. Only polygons whose every corner is, a game
 * frame later, beyond one edge of the screen: anything still on it that the
 * game no longer draws went for another reason (hidden, or a polygon held,
 * which a ghost of would show). */
static int leftovers(frame_blend_t *b, kept_t *cur) {
    kept_t *prev = kept_at(b, 1);
    size_t i, k;
    unsigned c, a;
    b->leftover_count = 0;
    if (!prev->valid || prev->number + 1 != cur->number || cur->reason == FRAME_BLEND_NO_HISTORY) return 1;
    if (!grow((void **)&b->continued, &b->continued_capacity, prev->polygon_count, sizeof(*b->continued))) return 0;
    if (prev->polygon_count) memset(b->continued, 0, prev->polygon_count);
    for (i = 0; i < cur->polygon_count; ++i) {
        int32_t r = predecessor(b, &cur->polygons[i]);
        if (r >= 0) b->continued[r] = 1;
    }
    /* Where each projection of the two frames before stands a game frame later. */
    for (a = 0; a < 2; ++a) {
        const kept_t *at = kept_at(b, a), *from = kept_at(b, a + 1);
        if (!grow((void **)&b->forward[a], &b->forward_capacity[a], from->projection_count + 1, sizeof(*b->forward[a]))) return 0;
        for (k = 0; k < from->projection_count; ++k) b->forward[a][k] = -1;
        if (!at->valid || !from->valid || at->number != from->number + 1) continue;
        for (k = 0; k < at->projection_count; ++k) {
            int32_t p = at->projections[k].previous;
            if (p >= 0 && (size_t)p < from->projection_count) b->forward[a][p] = (int32_t)k;
        }
    }
    for (i = 0; i < prev->polygon_count; ++i) {
        frame_identity_polygon_t q = prev->polygons[i];
        int32_t x0 = INT32_MAX, x1 = INT32_MIN, y0 = INT32_MAX, y1 = INT32_MIN;
        leftover_t *l;
        if (b->continued[i] || !prev->coloured[i] || !drawn(&q)) continue;
        for (c = 0; c < q.corners; ++c) {
            int32_t p;
            const kept_t *at;
            if (q.state[c] != FRAME_IDENTITY_LINKED || q.age[c] > 1 || q.projection[c] < 0) break;
            if ((p = b->forward[q.age[c]][q.projection[c]]) < 0) break;
            at = kept_at(b, q.age[c]);
            /* The corner as the latest frame would link it. */
            q.projection[c] = p; q.xy[c] = at->projections[p].sxy;
            x0 = low(q.xy[c]) < x0 ? low(q.xy[c]) : x0; x1 = low(q.xy[c]) > x1 ? low(q.xy[c]) : x1;
            y0 = high(q.xy[c]) < y0 ? high(q.xy[c]) : y0; y1 = high(q.xy[c]) > y1 ? high(q.xy[c]) : y1;
        }
        if (c < q.corners || !(x1 < 0 || y1 < 0 || x0 >= b->screen[0] || y0 >= b->screen[1])) continue;
        if (!grow((void **)&b->leftovers, &b->leftover_capacity, b->leftover_count + 1, sizeof(*b->leftovers)) ||
            !grow((void **)&b->vertices, &b->vertex_capacity, b->vertex_count + q.corners, sizeof(*b->vertices))) return 0;
        l = &b->leftovers[b->leftover_count];
        for (c = 0; c < q.corners; ++c) {
            vertex_t *v = &b->vertices[b->vertex_count + c];
            if (ends(b, &q, c, v) != ENDS_OK) break;
            l->vertex[c] = (int32_t)(b->vertex_count + c);
        }
        if (c < q.corners) continue;
        memcpy(l->words, prev->packets[i], sizeof(l->words));
        b->vertex_count += q.corners;
        ++b->leftover_count;
    }
    b->info.leftovers = (uint32_t)b->leftover_count;
    return 1;
}

int frame_blend_add(frame_blend_t *b, const frame_identity_frame_t *f) {
    kept_t *cur, *prev;
    frame_blend_info_t *info = &b->info;
    uint32_t i, c;
    int changed;
    b->latest = (b->latest + 1) % 3;
    cur = kept_at(b, 0); prev = kept_at(b, 1);
    memset(info, 0, sizeof(*info));
    info->number = f->number; info->vblank = f->vblank;
    info->first_polygon = f->first_polygon;
    b->vertex_count = 0;
    if (!keep(cur, f) || !grow((void **)&b->corners, &b->corner_capacity, f->polygon_count, sizeof(*b->corners)) ||
        !grow((void **)&b->codes, &b->code_capacity, f->polygon_count, sizeof(*b->codes)) ||
        !grow((void **)&b->hidden, &b->hidden_capacity, f->polygon_count, sizeof(*b->hidden)) ||
        !surfaces(b, f)) {
        cur->valid = 0;
        return 0;
    }
    attach_looks(b, cur, f);
    if (f->flags & (FRAME_IDENTITY_RESET | FRAME_IDENTITY_OVERFLOW)) cur->reason = FRAME_BLEND_NO_HISTORY;
    else if (f->flags & FRAME_IDENTITY_EXCLUDED) cur->reason = FRAME_BLEND_EXCLUDED;
    else if (!prev->valid || prev->number + 1 != f->number) cur->reason = FRAME_BLEND_NO_HISTORY;
    else if (prev->tag != f->tag) cur->reason = FRAME_BLEND_SCENE;
    else {
        if (!cut_measure(b, cur, prev, kept_at(b, 2), &info->move, &info->change)) { cur->valid = 0; return 0; }
        info->screen_move = screen_move(b, cur, prev);
        cur->reason = (info->move >= FRAME_BLEND_CUT_MOVE ||
                       (info->screen_move >= FRAME_BLEND_CUT_SCREEN && info->move >= FRAME_BLEND_CUT_NEAR)) &&
                      info->change * 2 >= info->move ? FRAME_BLEND_CUT : FRAME_BLEND_KEPT;
    }
    info->reason = cur->reason;
    if (!motion_plan(b, cur, prev)) { cur->valid = 0; return 0; }
    resolve(b, cur);
    if (!index_corners(cur) || !grow((void **)&b->before, &b->before_capacity, f->polygon_count, sizeof(*b->before))) {
        cur->valid = 0;
        return 0;
    }
    for (i = 0; i < f->polygon_count; ++i) {
        const frame_identity_polygon_t *q = &cur->polygons[i];
        b->before[i] = -1;
        for (c = 0; c < q->corners; ++c) if (q->state[c] != FRAME_IDENTITY_LINKED) break;
        if (c == q->corners && cur->coloured[i]) b->before[i] = predecessor(b, q);
        if (b->before[i] >= 0 && memcmp(prev->colours[b->before[i]], cur->colours[i], sizeof(cur->colours[i])))
            ++info->polygons_recoloured;
    }
    /* Every position of a 3D surface is one vertex. A corner that cannot move
     * pins its position; so do projections that share a position now but
     * stood apart before, which are not one vertex. */
    for (i = 0; i < f->polygon_count; ++i) {
        const frame_identity_polygon_t *q = &cur->polygons[i];
        vertex_t v[4];
        unsigned good = 0, three_d = 0;
        for (c = 0; c < 4; ++c) b->corners[i][c] = -1;
        for (c = 0; c < q->corners; ++c) {
            if (q->state[c] != FRAME_IDENTITY_NO_PROJECTION) three_d = 1;
            b->codes[i][c] = (uint8_t)ends(b, q, c, &v[c]);
            if (b->codes[i][c] == ENDS_OK) ++good;
        }
        b->hidden[i] = 0;
        if (!three_d) continue;
        /* What the GPU does not draw holds nothing: a polygon with a corner
         * clamped far off screen would otherwise hold its neighbours, and
         * through them, by the rule below, a whole floor. */
        b->hidden[i] = (uint8_t)!drawn(q);
        for (c = 0; c < q->corners; ++c) {
            int32_t at = vertex_at(b, q->xy[c], surface_of(b->surfaces, i));
            vertex_t *w;
            if (at < 0) { cur->valid = 0; return 0; }
            b->corners[i][c] = at;
            w = &b->vertices[at];
            if (good != q->corners) { if (!b->hidden[i]) w->pinned = 1; }
            else if (w->pinned < 0) *w = v[c];
            else if (!w->pinned && (abs(low(w->sxy[0]) - low(v[c].sxy[0])) > 1 ||
                                    abs(high(w->sxy[0]) - high(v[c].sxy[0])) > 1)) w->pinned = 1;
        }
    }
    /* A polygon with corners held and corners that move far would stretch:
     * hold it whole, and with it every position of its surface it shares, until
     * none is left. */
    for (changed = 1; changed;) {
        changed = 0;
        for (i = 0; i < f->polygon_count; ++i) {
            const frame_identity_polygon_t *q = &cur->polygons[i];
            unsigned held = 0, far = 0;
            if (b->corners[i][0] < 0 || b->hidden[i]) continue;
            for (c = 0; c < q->corners; ++c) {
                const vertex_t *w = &b->vertices[b->corners[i][c]];
                if (w->pinned) ++held;
                else if (abs(low(w->sxy[1]) - low(w->sxy[0])) > FRAME_BLEND_STRETCH ||
                         abs(high(w->sxy[1]) - high(w->sxy[0])) > FRAME_BLEND_STRETCH) ++far;
            }
            if (!held || !far) continue;
            for (c = 0; c < q->corners; ++c) b->vertices[b->corners[i][c]].pinned = 1;
            changed = 1;
        }
    }
    for (i = 0; i < f->polygon_count; ++i) {
        const frame_identity_polygon_t *q = &cur->polygons[i];
        unsigned moving = 0;
        if (b->corners[i][0] < 0) continue;
        ++info->polygons_3d; info->corners_3d += q->corners;
        for (c = 0; c < q->corners; ++c) {
            if (!b->vertices[b->corners[i][c]].pinned && !b->hidden[i]) { ++moving; continue; }
            switch (b->codes[i][c]) {
            case ENDS_OK: ++info->held_neighbour; break;
            case ENDS_SERVICE: ++info->held_service; break;
            case ENDS_PROJECTION: ++info->held_projection; break;
            case ENDS_JUMP: ++info->held_jump; break;
            default: ++info->held_reset; break;
            }
        }
        info->corners_moving += moving;
        if (moving == q->corners) ++info->polygons_moving;
        for (c = 0; c < q->corners; ++c)
            if (!b->vertices[b->corners[i][c]].pinned && !b->hidden[i] && b->vertices[b->corners[i][c]].run >= 0) ++info->corners_turning;
    }
    info->polygon_count = f->polygon_count;
    if (!leftovers(b, cur)) { cur->valid = 0; return 0; }
    return 1;
}

/* Integer interpolation that gives each end exactly. */
static int32_t between(int32_t a, int32_t b, uint32_t num, uint32_t den) {
    int64_t d = ((int64_t)b - a) * num;
    return (int32_t)(a + (d >= 0 ? (d + den / 2) / den : -((-d + den / 2) / (int64_t)den)));
}

/* How far a point lies from the segment between two others, squared. */
static double off_line(double x, double y, double ax, double ay, double bx, double by) {
    double dx = bx - ax, dy = by - ay, l = dx * dx + dy * dy, t = 0;
    if (l > 0) { t = ((x - ax) * dx + (y - ay) * dy) / l; t = t < 0 ? 0 : t > 1 ? 1 : t; }
    dx = x - (ax + t * dx); dy = y - (ay + t * dy);
    return dx * dx + dy * dy;
}

static int saturated(const vertex_t *v, uint32_t xy) {
    unsigned e, a;
    for (e = 0; e < 2; ++e)
        for (a = 0; a < 2; ++a)
            if (v->view[e][a] > 32767 || v->view[e][a] < (v->lm ? 0 : -32768)) return 1;
    return low(xy) <= -1024 || low(xy) >= 1023 || high(xy) <= -1024 || high(xy) >= 1023 ||
           low(v->sxy[0]) <= -1024 || low(v->sxy[0]) >= 1023 || high(v->sxy[0]) <= -1024 || high(v->sxy[0]) >= 1023 ||
           low(v->sxy[1]) <= -1024 || low(v->sxy[1]) >= 1023 || high(v->sxy[1]) <= -1024 || high(v->sxy[1]) >= 1023;
}

/* Where a vertex stands at alpha: its view-space position, projected again. */
static uint32_t place(const frame_blend_t *b, const vertex_t *v, uint32_t num, uint32_t den, int32_t view[3]) {
    unsigned a;
    if (v->run >= 0 && num && num < den) turned(kept_seen(b, v->age)->motions, v, num, den, view);
    else for (a = 0; a < 3; ++a) view[a] = between(v->view[0][a], v->view[1][a], num, den);
    return frame_blend_project(view, between(v->ofx[0], v->ofx[1], num, den), between(v->ofy[0], v->ofy[1], num, den),
                               (uint32_t)between((int32_t)v->h[0], (int32_t)v->h[1], num, den), v->shift, v->lm);
}

unsigned frame_blend_leftovers(const frame_blend_t *b) { return (unsigned)b->leftover_count; }

unsigned frame_blend_leftover(const frame_blend_t *b, unsigned index, uint32_t num, uint32_t den, uint32_t words[12]) {
    const leftover_t *l;
    unsigned op, corners, c, k = 1, n;
    if (index >= b->leftover_count || !den || num >= den) return 0;
    l = &b->leftovers[index];
    op = l->words[0] >> 24; corners = (op & 8) ? 4 : 3;
    n = 1 + corners * (1u + ((op & 4) ? 1u : 0u)) + ((op & 16) ? corners - 1 : 0);
    memcpy(words, l->words, sizeof(l->words));
    for (c = 0; c < corners; ++c) {
        int32_t view[3];
        if (c && (op & 16)) ++k;
        words[k++] = place(b, &b->vertices[l->vertex[c]], num, den, view);
        if (op & 4) ++k;
    }
    return n;
}

unsigned frame_blend_polygon(const frame_blend_t *b, uint64_t serial, uint32_t num, uint32_t den,
                             uint32_t xy[4], frame_blend_check_t *check) {
    uint64_t i = serial - b->info.first_polygon;
    unsigned c, mask = 0, corners = 0;
    int32_t at[4][3];
    const vertex_t *vs[4];
    if (serial < b->info.first_polygon || i >= b->info.polygon_count || !den || num > den || b->hidden[i]) return 0;
    for (c = 0; c < 4; ++c) {
        const vertex_t *v;
        int32_t *view = at[c];
        unsigned a;
        if (b->corners[i][c] < 0) break;
        ++corners;
        v = vs[c] = &b->vertices[b->corners[i][c]];
        if (v->pinned) continue;
        mask |= 1u << c;
        xy[c] = place(b, v, num, den, view);
        if (!check || saturated(v, xy[c])) continue;
        /* A straight line in view space is a straight line on screen, unless
         * the GTE saturates IR or SXY on the way: far off screen, where its
         * own frames are not on a line either. A corner that turns with its
         * part leaves that line, but by no more than an arc of at most half a
         * turn between its ends leaves its chord, plus its turn about its own
         * part's origin. */
        {
            double x = low(xy[c]), y = high(xy[c]), x0 = low(v->sxy[0]), y0 = high(v->sxy[0]), x1 = low(v->sxy[1]), y1 = high(v->sxy[1]);
            double limit = 2;
            if (v->run >= 0) {
                const kept_t *k = kept_seen(b, v->age);
                double reach = 0, depth = v->shift ? v->view[1][2] : v->view[1][2] / 4096.0;
                int32_t r = v->run;
                unsigned n = 0;
                /* Its farthest distance from the origin of a part that carries
                 * it, on screen at the end. */
                for (; r >= 0 && n < 64; r = k->motions[r].parent, ++n) {
                    double d = 0;
                    for (a = 0; a < 3; ++a) d += (v->view[1][a] - k->motions[r].t[1][a]) * (v->view[1][a] - k->motions[r].t[1][a]);
                    if (d > reach) reach = d;
                }
                if (depth > 0) reach = root(reach) * (v->h[1] & 65535u) / depth;
                limit += root((x1 - x0) * (x1 - x0) + (y1 - y0) * (y1 - y0)) / 2 + reach;
            }
            if (off_line(x, y, x0, y0, x1, y1) > limit * limit) ++check->stray;
        }
    }
    if (check && mask && mask == (1u << corners) - 1) {
        /* A part neither shrinks nor grows between its two ends: each edge in
         * view space stays within a tenth of its lengths at the two ends. */
        static const unsigned triangle[3][2] = {{0, 1}, {1, 2}, {2, 0}}, quad[4][2] = {{0, 1}, {1, 3}, {3, 2}, {2, 0}};
        unsigned e, resized = 0;
        ++check->polygons;
        for (e = 0; e < corners; ++e) {
            unsigned u = corners == 4 ? quad[e][0] : triangle[e][0], w = corners == 4 ? quad[e][1] : triangle[e][1], a;
            double l = 0, l0 = 0, l1 = 0, lo, hi;
            for (a = 0; a < 3; ++a) {
                double d = (double)at[u][a] - at[w][a], d0 = (double)vs[u]->view[0][a] - vs[w]->view[0][a],
                       d1 = (double)vs[u]->view[1][a] - vs[w]->view[1][a];
                l += d * d; l0 += d0 * d0; l1 += d1 * d1;
            }
            l = root(l); l0 = root(l0); l1 = root(l1);
            lo = l0 < l1 ? l0 : l1; hi = l0 < l1 ? l1 : l0;
            if (l < lo * 0.9 - 2 || l > hi * 1.1 + 2) resized = 1;
        }
        check->resized += resized;
    }
    return mask;
}

unsigned frame_blend_colours(const frame_blend_t *b, uint64_t serial, uint32_t num, uint32_t den, uint32_t colours[4]) {
    const kept_t *cur = kept_seen(b, 0), *prev = kept_seen(b, 1);
    uint64_t i = serial - b->info.first_polygon;
    const uint32_t *from, *to;
    unsigned c, a, mask = 0, op;
    if (serial < b->info.first_polygon || i >= b->info.polygon_count || !den || num > den || b->before[i] < 0 ||
        cur->polygons[i].age[0] > 1 || kept_seen(b, cur->polygons[i].age[0])->reason != FRAME_BLEND_KEPT) return 0;
    op = cur->polygons[i].opcode;
    from = prev->colours[b->before[i]]; to = cur->colours[i];
    /* Raw textures ignore the colour; a change of blending mode between the
     * two (semi-transparency on a textured polygon) cannot be drawn between. */
    if (!prev->coloured[b->before[i]] || ((op & 4) && ((op & 1) || ((prev->pages[b->before[i]] ^ cur->pages[i]) & 0x60u))))
        return 0;
    for (c = 0; c < cur->polygons[i].corners; ++c) {
        uint32_t out = 0;
        if (from[c] == to[c]) { colours[c] = to[c]; continue; }
        for (a = 0; a < 24; a += 8)
            out |= (uint32_t)between((int32_t)((from[c] >> a) & 255u), (int32_t)((to[c] >> a) & 255u), num, den) << a;
        colours[c] = out;
        mask |= 1u << c;
    }
    return mask;
}
