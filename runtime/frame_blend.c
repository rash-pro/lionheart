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
    size_t projection_count, run_count, setup_count;
    size_t projection_capacity, run_capacity, setup_capacity, jumped_capacity;
} kept_t;

/* Both ends of a vertex: where it stood in the frame before (a) and now (b). */
typedef struct {
    int32_t view[2][3];
    int32_t ofx[2], ofy[2];
    uint32_t h[2], sxy[2];
    uint32_t shift, lm;
    int pinned;
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
    uint32_t *keys;
    int32_t *slots;
    size_t table_capacity;
    double *moves;               /* four arrays of one value per run */
    uint64_t histogram[2048];
    size_t move_capacity;
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

frame_blend_t *frame_blend_create(void) { return calloc(1, sizeof(frame_blend_t)); }

void frame_blend_destroy(frame_blend_t *b) {
    unsigned i;
    if (!b) return;
    for (i = 0; i < 3; ++i) {
        free(b->kept[i].projections); free(b->kept[i].runs); free(b->kept[i].setups); free(b->kept[i].jumped);
    }
    free(b->corners); free(b->codes); free(b->vertices); free(b->keys); free(b->slots); free(b->moves);
    free(b);
}

void frame_blend_reset(frame_blend_t *b) {
    unsigned i;
    for (i = 0; i < 3; ++i) b->kept[i].valid = 0;
    memset(&b->info, 0, sizeof(b->info));
}

const frame_blend_info_t *frame_blend_info(const frame_blend_t *b) { return &b->info; }

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

static int keep(kept_t *k, const frame_identity_frame_t *f) {
    if (!grow((void **)&k->projections, &k->projection_capacity, f->projection_count, sizeof(*k->projections)) ||
        !grow((void **)&k->runs, &k->run_capacity, f->run_count, sizeof(*k->runs)) ||
        !grow((void **)&k->setups, &k->setup_capacity, f->setup_count, sizeof(*k->setups)) ||
        !grow((void **)&k->jumped, &k->jumped_capacity, f->run_count, sizeof(*k->jumped))) return 0;
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

static size_t table_slot(const frame_blend_t *b, uint32_t key) {
    size_t i = (size_t)((key * UINT32_C(2654435761)) & (b->table_capacity - 1));
    while (b->slots[i] >= 0 && b->keys[i] != key) i = (i + 1) & (b->table_capacity - 1);
    return i;
}

static int table_reset(frame_blend_t *b, size_t entries) {
    size_t n = 64;
    while (n < entries * 2) n *= 2;
    if (n > b->table_capacity) {
        uint32_t *keys = realloc(b->keys, n * sizeof(*keys));
        int32_t *slots;
        if (!keys) return 0;
        b->keys = keys;
        if (!(slots = realloc(b->slots, n * sizeof(*slots)))) return 0;
        b->slots = slots; b->table_capacity = n;
    }
    memset(b->slots, 0xff, b->table_capacity * sizeof(*b->slots));
    return 1;
}

/* The vertex at a packet position, made if there is none. */
static int32_t vertex_at(frame_blend_t *b, uint32_t xy) {
    size_t i = table_slot(b, xy);
    if (b->slots[i] >= 0) return b->slots[i];
    if (!grow((void **)&b->vertices, &b->vertex_capacity, b->vertex_count + 1, sizeof(*b->vertices))) return -1;
    memset(&b->vertices[b->vertex_count], 0, sizeof(*b->vertices));
    b->vertices[b->vertex_count].pinned = -1; /* not yet given ends */
    b->keys[i] = xy; b->slots[i] = (int32_t)b->vertex_count;
    return (int32_t)b->vertex_count++;
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
    return ENDS_OK;
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
        !table_reset(b, (size_t)f->polygon_count * 4)) {
        cur->valid = 0;
        return 0;
    }
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
    /* Every position of a 3D polygon is one vertex. A corner that cannot move
     * pins its position; so do projections that share a position now but
     * stood apart before, which are not one vertex. */
    for (i = 0; i < f->polygon_count; ++i) {
        const frame_identity_polygon_t *q = &f->polygons[i];
        vertex_t v[4];
        unsigned good = 0, three_d = 0;
        for (c = 0; c < 4; ++c) b->corners[i][c] = -1;
        for (c = 0; c < q->corners; ++c) {
            if (q->state[c] != FRAME_IDENTITY_NO_PROJECTION) three_d = 1;
            b->codes[i][c] = (uint8_t)ends(b, q, c, &v[c]);
            if (b->codes[i][c] == ENDS_OK) ++good;
        }
        if (!three_d) continue;
        for (c = 0; c < q->corners; ++c) {
            int32_t at = vertex_at(b, q->xy[c]);
            vertex_t *w;
            if (at < 0) { cur->valid = 0; return 0; }
            b->corners[i][c] = at;
            w = &b->vertices[at];
            if (good != q->corners) w->pinned = 1;
            else if (w->pinned < 0) *w = v[c];
            else if (!w->pinned && (abs(low(w->sxy[0]) - low(v[c].sxy[0])) > 1 ||
                                    abs(high(w->sxy[0]) - high(v[c].sxy[0])) > 1)) w->pinned = 1;
        }
    }
    /* A polygon with corners held and corners that move far would stretch:
     * hold it whole, and with it every position it shares, until none is left. */
    for (changed = 1; changed;) {
        changed = 0;
        for (i = 0; i < f->polygon_count; ++i) {
            const frame_identity_polygon_t *q = &f->polygons[i];
            unsigned held = 0, far = 0;
            if (b->corners[i][0] < 0) continue;
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
        const frame_identity_polygon_t *q = &f->polygons[i];
        unsigned moving = 0;
        if (b->corners[i][0] < 0) continue;
        ++info->polygons_3d; info->corners_3d += q->corners;
        for (c = 0; c < q->corners; ++c) {
            if (!b->vertices[b->corners[i][c]].pinned) { ++moving; continue; }
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
    }
    info->polygon_count = f->polygon_count;
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

unsigned frame_blend_polygon(const frame_blend_t *b, uint64_t serial, uint32_t num, uint32_t den,
                             uint32_t xy[4], unsigned *stray) {
    uint64_t i = serial - b->info.first_polygon;
    unsigned c, mask = 0;
    if (serial < b->info.first_polygon || i >= b->info.polygon_count || !den || num > den) return 0;
    for (c = 0; c < 4; ++c) {
        const vertex_t *v;
        int32_t view[3];
        unsigned a;
        if (b->corners[i][c] < 0) break;
        v = &b->vertices[b->corners[i][c]];
        if (v->pinned) continue;
        for (a = 0; a < 3; ++a) view[a] = between(v->view[0][a], v->view[1][a], num, den);
        xy[c] = frame_blend_project(view, between(v->ofx[0], v->ofx[1], num, den), between(v->ofy[0], v->ofy[1], num, den),
                                    (uint32_t)between((int32_t)v->h[0], (int32_t)v->h[1], num, den), v->shift, v->lm);
        mask |= 1u << c;
        /* A straight line in view space is a straight line on screen, unless
         * the GTE saturates IR or SXY on the way: far off screen, where its
         * own frames are not on a line either. */
        if (stray && !saturated(v, xy[c]) &&
            off_line(low(xy[c]), high(xy[c]), low(v->sxy[0]), high(v->sxy[0]), low(v->sxy[1]), high(v->sxy[1])) > 4.0)
            ++*stray;
    }
    return mask;
}
