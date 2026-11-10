#include "frame_identity.h"
#include "gpu.h"
#include "gte.h"
#include <stdlib.h>
#include <string.h>

/* Beyond this a frame is not paired; a few thousand is usual. */
enum { LIMIT = 1 << 20 };

/* Open addressing from a 64-bit key to the first index of a chain. */
typedef struct {
    uint64_t *keys;
    int32_t *heads;
    size_t capacity;
} table_t;

/* One game frame as it is recorded, and kept once ended as the previous one. */
typedef struct {
    frame_identity_setup_t *setups;
    frame_identity_run_t *runs;
    frame_identity_projection_t *projections;
    frame_identity_polygon_t *polygons;
    uint32_t *drawn_at;         /* projections recorded when each polygon was drawn */
    uint32_t *previous_sxy;     /* where each matched projection stood in the frame before */
    int32_t *same_sxy;          /* next projection at the same position */
    int32_t *same_key;          /* next run with the same key */
    size_t setup_count, run_count, projection_count, polygon_count;
    size_t setup_capacity, run_capacity, projection_capacity, polygon_capacity;
    size_t drawn_capacity, previous_capacity, sxy_capacity, key_capacity;
    table_t by_sxy, by_key;
    uint64_t first_projection, first_polygon;
    uint32_t flags;
    int valid;                  /* ended and kept as history for the next frame */
} segment_t;

static struct {
    int running;
    gpu_t *gpu;
    frame_identity_config_t config;
    segment_t segments[2];
    segment_t *current, *previous;
    uint32_t display_start;
    int have_start, reset_pending, have_end;
    uint64_t vblanks, end_vblank, frames, projections, polygons;
    const cpu_t *cpu;
} s;

static void display_start(void *context, uint32_t start);
static void vblank(void *context);
static void polygon(void *context, unsigned opcode, const uint32_t *packet);
static const gpu_observer_t observer = {display_start, vblank, polygon};

static int16_t low(uint32_t word) { return (int16_t)(uint16_t)(word & 0xffffu); }
static int16_t high(uint32_t word) { return (int16_t)(uint16_t)(word >> 16); }

static uint64_t mix(uint64_t h, uint64_t v) {
    h ^= v + UINT64_C(0x9e3779b97f4a7c15) + (h << 6) + (h >> 2);
    return h * UINT64_C(0xff51afd7ed558ccd);
}

static int grow(void **items, size_t *capacity, size_t needed, size_t size) {
    size_t n = *capacity ? *capacity : 256;
    void *p;
    if (needed <= *capacity) return 1;
    while (n < needed) n *= 2;
    if (!(p = realloc(*items, n * size))) return 0;
    *items = p; *capacity = n;
    return 1;
}

static int table_reset(table_t *t, size_t entries) {
    size_t n = 64;
    while (n < entries * 2) n *= 2;
    if (n > t->capacity) {
        uint64_t *keys = realloc(t->keys, n * sizeof(*keys));
        int32_t *heads;
        if (!keys) return 0;
        t->keys = keys;
        if (!(heads = realloc(t->heads, n * sizeof(*heads)))) return 0;
        t->heads = heads; t->capacity = n;
    }
    memset(t->heads, 0xff, t->capacity * sizeof(*t->heads));
    return 1;
}

static size_t slot(const table_t *t, uint64_t key) {
    size_t i = (size_t)(mix(0, key) & (t->capacity - 1));
    while (t->heads[i] >= 0 && t->keys[i] != key) i = (i + 1) & (t->capacity - 1);
    return i;
}

static int32_t table_get(const table_t *t, uint64_t key) {
    return t->capacity ? t->heads[slot(t, key)] : -1;
}

/* Chains are built from the end, so each lists its items in recorded order. */
static void table_chain(table_t *t, uint64_t key, int32_t index, int32_t *next) {
    size_t i = slot(t, key);
    t->keys[i] = key;
    next[index] = t->heads[i];
    t->heads[i] = index;
}

static void segment_clear(segment_t *g) {
    g->setup_count = g->run_count = g->projection_count = g->polygon_count = 0;
    g->flags = 0; g->valid = 0;
}

static void segment_free(segment_t *g) {
    free(g->setups); free(g->runs); free(g->projections); free(g->polygons);
    free(g->drawn_at); free(g->previous_sxy); free(g->same_sxy); free(g->same_key);
    free(g->by_sxy.keys); free(g->by_sxy.heads); free(g->by_key.keys); free(g->by_key.heads);
    memset(g, 0, sizeof(*g));
}

static void begin(segment_t *g) {
    segment_clear(g);
    g->first_projection = s.projections;
    g->first_polygon = s.polygons;
}

int frame_identity_start(gpu_t *gpu, const frame_identity_config_t *config) {
    frame_identity_stop();
    memset(&s, 0, sizeof(s));
    s.config = *config;
    if (!s.config.repeat_pixels) s.config.repeat_pixels = 8;
    s.current = &s.segments[0]; s.previous = &s.segments[1];
    begin(s.current);
    s.running = 1;
    frame_identity_rebind(gpu);
    return 1;
}

void frame_identity_rebind(gpu_t *gpu) {
    if (!s.running) return;
    if (s.gpu && s.gpu != gpu) gpu_observe(s.gpu, NULL, NULL);
    s.gpu = gpu;
    if (gpu) gpu_observe(gpu, &observer, NULL);
    frame_identity_reset();
}

void frame_identity_reset(void) {
    if (!s.running) return;
    /* A restored state starts a new game frame with no history behind it. */
    s.previous->valid = 0;
    begin(s.current);
    s.have_start = 0;
    s.reset_pending = 1;
    s.cpu = NULL;
}

void frame_identity_stop(void) {
    if (s.gpu) gpu_observe(s.gpu, NULL, NULL);
    segment_free(&s.segments[0]);
    segment_free(&s.segments[1]);
    memset(&s, 0, sizeof(s));
}

int frame_identity_running(void) { return s.running; }

frame_identity_totals_t frame_identity_totals(void) {
    frame_identity_totals_t t;
    t.frames = s.frames; t.projections = s.projections; t.polygons = s.polygons;
    return t;
}

/* Recording. */
static void record(cpu_t *c, unsigned v, unsigned shift, int lm, uint32_t sxy, uint32_t sz) {
    segment_t *g = s.current;
    frame_identity_setup_t setup;
    frame_identity_projection_t *p;
    const uint32_t *k = c->gte_ctrl;
    int32_t r[9], in[3];
    unsigned i;
    s.cpu = c;
    ++s.projections;
    if (g->flags & FRAME_IDENTITY_OVERFLOW) return;
    if (g->projection_count == LIMIT) { g->flags |= FRAME_IDENTITY_OVERFLOW; return; }
    memset(&setup, 0, sizeof(setup));
    for (i = 0; i < 5; ++i) setup.rotation[i] = k[i];
    for (i = 0; i < 3; ++i) setup.translation[i] = (int32_t)k[5 + i];
    setup.ofx = (int32_t)k[24]; setup.ofy = (int32_t)k[25]; setup.h = k[26] & 0xffffu;
    setup.shift = shift; setup.lm = lm != 0;
    if (!g->setup_count || memcmp(&g->setups[g->setup_count - 1], &setup, sizeof(setup))) {
        if (!grow((void **)&g->setups, &g->setup_capacity, g->setup_count + 1, sizeof(setup))) {
            g->flags |= FRAME_IDENTITY_OVERFLOW; return;
        }
        g->setups[g->setup_count++] = setup;
    }
    if (!grow((void **)&g->projections, &g->projection_capacity, g->projection_count + 1, sizeof(*p))) {
        g->flags |= FRAME_IDENTITY_OVERFLOW; return;
    }
    p = &g->projections[g->projection_count++];
    in[0] = low(c->gte_data[v * 2]); in[1] = high(c->gte_data[v * 2]); in[2] = low(c->gte_data[v * 2 + 1]);
    for (i = 0; i < 3; ++i) p->input[i] = (int16_t)in[i];
    for (i = 0; i < 9; ++i) r[i] = i & 1 ? high(k[i / 2]) : low(k[i / 2]);
    for (i = 0; i < 3; ++i) {
        int64_t n = (int64_t)setup.translation[i] * 4096 + (int64_t)r[i * 3] * in[0] +
                    (int64_t)r[i * 3 + 1] * in[1] + (int64_t)r[i * 3 + 2] * in[2];
        /* An arithmetic shift, as the GTE's, then MAC's 32 bits. */
        n = n >= 0 ? (int64_t)((uint64_t)n >> shift) : -1 - (int64_t)((uint64_t)(-1 - n) >> shift);
        p->view[i] = (int32_t)(uint32_t)(uint64_t)n;
    }
    p->sxy = sxy; p->sz = (uint16_t)sz;
    p->run = (uint32_t)(g->setup_count - 1); /* the setup for now; runs are formed at the end */
    p->previous = p->match = FRAME_IDENTITY_HELD;
}

void frame_identity_rtps(cpu_t *c, unsigned shift, int lm) {
    gte_rtps(c, shift, lm);
    if (s.running) record(c, 0, shift, lm, c->gte_data[14], c->gte_data[19]);
}

void frame_identity_rtpt(cpu_t *c, unsigned shift, int lm) {
    unsigned i;
    gte_rtpt(c, shift, lm);
    /* The FIFOs hold the three results in order once RTPT is done. */
    if (s.running) for (i = 0; i < 3; ++i) record(c, i, shift, lm, c->gte_data[12 + i], c->gte_data[17 + i]);
}

void frame_identity_gte_command(cpu_t *c, uint32_t opcode) {
    unsigned shift = (opcode & (1u << 19)) ? 12 : 0;
    int lm = (opcode & (1u << 10)) != 0;
    if ((opcode & 63) == 0x01) frame_identity_rtps(c, shift, lm);
    else if ((opcode & 63) == 0x30) frame_identity_rtpt(c, shift, lm);
    else gte_cmd(c, opcode);
}

static void polygon(void *context, unsigned opcode, const uint32_t *packet) {
    segment_t *g = s.current;
    frame_identity_polygon_t *q;
    unsigned n = (opcode & 8) ? 4 : 3, i, at = 1;
    (void)context;
    ++s.polygons;
    if (g->flags & FRAME_IDENTITY_OVERFLOW) return;
    if (g->polygon_count == LIMIT ||
        !grow((void **)&g->polygons, &g->polygon_capacity, g->polygon_count + 1, sizeof(*q)) ||
        !grow((void **)&g->drawn_at, &g->drawn_capacity, g->polygon_count + 1, sizeof(*g->drawn_at))) {
        g->flags |= FRAME_IDENTITY_OVERFLOW; return;
    }
    q = &g->polygons[g->polygon_count];
    memset(q, 0, sizeof(*q));
    q->opcode = opcode; q->corners = n;
    for (i = 0; i < n; ++i) {
        if (i && (opcode & 16)) ++at;
        q->xy[i] = packet[at++];
        if (opcode & 4) ++at;
        q->projection[i] = FRAME_IDENTITY_HELD;
    }
    g->drawn_at[g->polygon_count++] = (uint32_t)g->projection_count;
}

static void vblank(void *context) { (void)context; ++s.vblanks; }

/* Ending a frame. */
static int same_inputs(const segment_t *a, const frame_identity_run_t *x, const segment_t *b,
                       const frame_identity_run_t *y) {
    uint32_t k;
    if (x->count != y->count) return 0;
    for (k = 0; k < x->count; ++k)
        if (memcmp(a->projections[x->first + k].input, b->projections[y->first + k].input,
                   sizeof(a->projections[0].input))) return 0;
    return 1;
}

/* Newton's method from above, so that the result never depends on a math
 * library: the stream must be the same wherever the player runs. */
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

/* How far two runs are apart: their translations and their first vertices
 * in view space, both counted, so that two parts placed alike by different
 * transforms still differ. */
static double distance(const segment_t *a, const frame_identity_run_t *x, const segment_t *b, const frame_identity_run_t *y) {
    const frame_identity_projection_t *p = &a->projections[x->first], *q = &b->projections[y->first];
    double d = 0;
    unsigned i;
    for (i = 0; i < 3; ++i) {
        double t = (double)a->setups[x->setup].translation[i] - (double)b->setups[y->setup].translation[i];
        double v = (double)p->view[i] - (double)q->view[i];
        d += t * t + v * v;
    }
    return root(d);
}

static int form_runs(segment_t *g) {
    size_t i = 0;
    if (!grow((void **)&g->same_sxy, &g->sxy_capacity, g->projection_count, sizeof(*g->same_sxy)) ||
        !table_reset(&g->by_sxy, g->projection_count)) return 0;
    while (i < g->projection_count) {
        size_t j = i + 1, k;
        frame_identity_run_t *r;
        uint64_t key;
        while (j < g->projection_count && g->projections[j].run == g->projections[i].run) ++j;
        if (!grow((void **)&g->runs, &g->run_capacity, g->run_count + 1, sizeof(*r))) return 0;
        r = &g->runs[g->run_count];
        r->first = (uint32_t)i; r->count = (uint32_t)(j - i); r->setup = g->projections[i].run;
        r->previous = r->match = FRAME_IDENTITY_HELD; r->repeats = 0;
        key = mix(0, r->count);
        for (k = i; k < j; ++k) {
            const int16_t *v = g->projections[k].input;
            key = mix(key, (uint64_t)(uint16_t)v[0] | (uint64_t)(uint16_t)v[1] << 16 | (uint64_t)(uint16_t)v[2] << 32);
            g->projections[k].run = (uint32_t)g->run_count;
        }
        r->key = key;
        ++g->run_count;
        i = j;
    }
    if (!grow((void **)&g->same_key, &g->key_capacity, g->run_count, sizeof(*g->same_key)) ||
        !table_reset(&g->by_key, g->run_count)) return 0;
    for (i = g->run_count; i-- > 0;) table_chain(&g->by_key, g->runs[i].key, (int32_t)i, g->same_key);
    for (i = g->projection_count; i-- > 0;) table_chain(&g->by_sxy, g->projections[i].sxy, (int32_t)i, g->same_sxy);
    return 1;
}

/* The runs with exactly this run's inputs, in a frame, through its key chain. */
static size_t equals(const segment_t *g, const segment_t *from, const frame_identity_run_t *r, int32_t *out, size_t max) {
    size_t n = 0;
    int32_t i;
    for (i = table_get(&g->by_key, r->key); i >= 0; i = g->same_key[i])
        if (same_inputs(g, &g->runs[i], from, r)) { if (n < max) out[n] = i; ++n; }
    return n;
}

static int pair_runs(segment_t *cur, const segment_t *prev) {
    size_t i;
    int32_t *a = NULL, *b = NULL;
    size_t capacity = 0;
    for (i = 0; i < cur->run_count; ++i) {
        frame_identity_run_t *r = &cur->runs[i];
        size_t na, nb, x;
        int32_t match = FRAME_IDENTITY_HELD;
        nb = equals(cur, cur, r, b, capacity);
        na = equals(prev, cur, r, a, capacity);
        if (na > capacity || nb > capacity) {
            size_t n = na > nb ? na : nb;
            int32_t *pa = realloc(a, n * sizeof(*a)), *pb;
            if (!pa) { free(b); free(a); return 0; }
            a = pa;
            if (!(pb = realloc(b, n * sizeof(*b)))) { free(b); free(a); return 0; }
            b = pb; capacity = n;
            nb = equals(cur, cur, r, b, capacity);
            na = equals(prev, cur, r, a, capacity);
        }
        r->repeats = (uint32_t)(nb - 1);
        if (!na) continue;
        if (na == 1 && nb == 1) match = a[0];
        else {
            /* Repeated content: identical instances, particles. The nearest
             * both ways, at most half as far as the next, and close on screen;
             * anything less is held. */
            const frame_identity_projection_t *mine = &cur->projections[r->first], *theirs;
            double best = 1e300, second = 1e300, back = 1e300, back_second = 1e300;
            int32_t arg = -1, back_arg = -1;
            int dx, dy;
            for (x = 0; x < na; ++x) {
                double d = distance(prev, &prev->runs[a[x]], cur, r);
                if (d < best) { second = best; best = d; arg = a[x]; } else if (d < second) second = d;
            }
            theirs = &prev->projections[prev->runs[arg].first];
            for (x = 0; x < nb; ++x) {
                double d = distance(prev, &prev->runs[arg], cur, &cur->runs[b[x]]);
                if (d < back) { back_second = back; back = d; back_arg = b[x]; } else if (d < back_second) back_second = d;
            }
            dx = low(theirs->sxy) - low(mine->sxy); dy = high(theirs->sxy) - high(mine->sxy);
            if (back_arg == (int32_t)i && best * 2 + 1 < second && back * 2 + 1 < back_second &&
                (double)dx * dx + (double)dy * dy <= (double)s.config.repeat_pixels * s.config.repeat_pixels)
                match = arg;
        }
        if (match < 0) continue;
        r->match = match;
        for (x = 0; x < r->count; ++x)
            cur->projections[r->first + x].match = (int32_t)(prev->runs[match].first + x);
    }
    free(a); free(b);
    return 1;
}

static int within_pixel(const uint32_t *positions, size_t n) {
    int minx = INT32_MAX, maxx = INT32_MIN, miny = INT32_MAX, maxy = INT32_MIN;
    size_t i;
    for (i = 0; i < n; ++i) {
        int x = low(positions[i]), y = high(positions[i]);
        if (x < minx) minx = x;
        if (x > maxx) maxx = x;
        if (y < miny) miny = y;
        if (y > maxy) maxy = y;
    }
    return maxx - minx <= 1 && maxy - miny <= 1;
}

enum { CANDIDATES = 64 };

/* Projections at a position made before a limit, oldest first. */
static size_t at_position(const segment_t *g, uint32_t xy, uint32_t limit, int32_t *out) {
    size_t n = 0;
    int32_t i;
    for (i = table_get(&g->by_sxy, xy); i >= 0 && (uint32_t)i < limit; i = g->same_sxy[i])
        if (n < CANDIDATES) out[n++] = i;
    return n;
}

static void associate(frame_identity_polygon_t *q, uint32_t drawn_at) {
    const segment_t *cur = s.current, *prev = s.previous->valid ? s.previous : NULL, *g = NULL;
    int32_t found[4][CANDIDATES], common[CANDIDATES];
    size_t count[4] = {0, 0, 0, 0}, ncommon = 0, c, k, x;
    uint32_t limit = 0;
    unsigned in_cur = 0, in_prev = 0, n = q->corners;
    uint8_t age = 0, state;
    for (c = 0; c < n; ++c) {
        if (at_position(cur, q->xy[c], drawn_at, found[c])) ++in_cur;
        if (prev && table_get(&prev->by_sxy, q->xy[c]) >= 0) ++in_prev;
    }
    /* The newest frame that holds every corner made the polygon. */
    if (in_cur == n) { g = cur; limit = drawn_at; }
    else if (prev && in_prev == n) { g = prev; limit = (uint32_t)prev->projection_count; age = 1; }
    if (!g) {
        state = in_cur || in_prev ? FRAME_IDENTITY_SPLIT : FRAME_IDENTITY_NO_PROJECTION;
        for (c = 0; c < n; ++c) q->state[c] = state;
        return;
    }
    for (c = 0; c < n; ++c) count[c] = at_position(g, q->xy[c], limit, found[c]);
    /* The transform runs every corner has a projection in. */
    for (k = 0; k < count[0]; ++k) {
        uint32_t run = g->projections[found[0][k]].run;
        int everywhere = 1, seen = 0;
        for (x = 0; x < ncommon; ++x) if ((uint32_t)common[x] == run) seen = 1;
        for (c = 1; c < n && everywhere; ++c) {
            int here = 0;
            for (x = 0; x < count[c]; ++x) if (g->projections[found[c][x]].run == run) here = 1;
            everywhere = here;
        }
        if (everywhere && !seen) common[ncommon++] = (int32_t)run;
    }
    for (c = 0; c < n; ++c) {
        uint32_t before[CANDIDATES];
        int32_t chosen = -1;
        size_t m = 0;
        state = FRAME_IDENTITY_LINKED;
        for (k = 0; k < count[c]; ++k) if (g->projections[found[c][k]].match < 0) state = FRAME_IDENTITY_UNPAIRED;
        if (state == FRAME_IDENTITY_LINKED) {
            for (k = 0; k < count[c]; ++k) before[k] = g->previous_sxy[found[c][k]];
            if (within_pixel(before, count[c])) {
                /* Every projection here moved together; prefer the polygon's run. */
                chosen = found[c][count[c] - 1];
                if (ncommon == 1)
                    for (k = 0; k < count[c]; ++k)
                        if (g->projections[found[c][k]].run == (uint32_t)common[0]) chosen = found[c][k];
            } else if (ncommon == 1) {
                for (k = 0; k < count[c]; ++k)
                    if (g->projections[found[c][k]].run == (uint32_t)common[0]) {
                        before[m++] = g->previous_sxy[found[c][k]];
                        chosen = found[c][k];
                    }
                if (!within_pixel(before, m)) chosen = -1;
            }
            if (chosen < 0) state = FRAME_IDENTITY_AMBIGUOUS;
        }
        if (state == FRAME_IDENTITY_LINKED) {
            q->age[c] = age; q->projection[c] = chosen;
            if (g->flags & (FRAME_IDENTITY_RESET | FRAME_IDENTITY_OVERFLOW)) state = FRAME_IDENTITY_NO_HISTORY;
            else if (g->flags & FRAME_IDENTITY_EXCLUDED) state = FRAME_IDENTITY_EXCLUDED_CORNER;
            if (state == FRAME_IDENTITY_NO_HISTORY) q->projection[c] = FRAME_IDENTITY_HELD;
        } else if (g->flags & (FRAME_IDENTITY_RESET | FRAME_IDENTITY_OVERFLOW)) state = FRAME_IDENTITY_NO_HISTORY;
        q->state[c] = state;
    }
}

static void end_frame(void) {
    segment_t *cur = s.current, *prev = s.previous;
    frame_identity_frame_t f;
    size_t i;
    uint64_t gap = s.vblanks - s.end_vblank;
    uint32_t tag = s.config.classify ? s.config.classify(s.config.context, s.cpu) : 0;
    int fresh = s.reset_pending || !s.have_end || !prev->valid || gap > FRAME_IDENTITY_GAP ||
                (prev->flags & FRAME_IDENTITY_OVERFLOW);
    if (!(cur->flags & FRAME_IDENTITY_OVERFLOW) &&
        (!form_runs(cur) || !grow((void **)&cur->previous_sxy, &cur->previous_capacity, cur->projection_count,
                                  sizeof(*cur->previous_sxy))))
        cur->flags |= FRAME_IDENTITY_OVERFLOW;
    if (cur->flags & FRAME_IDENTITY_OVERFLOW) {
        /* Nothing of an incomplete frame is trusted, and nothing links into it. */
        cur->run_count = 0;
        for (i = 0; i < cur->projection_count; ++i) cur->projections[i].run = UINT32_MAX;
        if (!table_reset(&cur->by_sxy, 0) || !table_reset(&cur->by_key, 0)) cur->by_sxy.capacity = cur->by_key.capacity = 0;
    }
    if (fresh) cur->flags |= FRAME_IDENTITY_RESET;
    if (tag & FRAME_IDENTITY_EXCLUDE) cur->flags |= FRAME_IDENTITY_EXCLUDED;
    if (!(cur->flags & (FRAME_IDENTITY_RESET | FRAME_IDENTITY_OVERFLOW)) && !pair_runs(cur, prev))
        cur->flags |= FRAME_IDENTITY_OVERFLOW;
    if (cur->flags & FRAME_IDENTITY_OVERFLOW)
        for (i = 0; i < cur->run_count; ++i) cur->runs[i].match = FRAME_IDENTITY_HELD;
    for (i = 0; i < cur->projection_count; ++i) {
        frame_identity_projection_t *p = &cur->projections[i];
        if (cur->flags & FRAME_IDENTITY_OVERFLOW) p->match = FRAME_IDENTITY_HELD;
        if (!(cur->flags & FRAME_IDENTITY_OVERFLOW))
            cur->previous_sxy[i] = p->match >= 0 ? prev->projections[p->match].sxy : 0;
        p->previous = cur->flags & (FRAME_IDENTITY_EXCLUDED | FRAME_IDENTITY_OVERFLOW) ? FRAME_IDENTITY_HELD : p->match;
    }
    for (i = 0; i < cur->run_count; ++i)
        cur->runs[i].previous = cur->flags & FRAME_IDENTITY_EXCLUDED ? FRAME_IDENTITY_HELD : cur->runs[i].match;
    if (!(cur->flags & FRAME_IDENTITY_OVERFLOW))
        for (i = 0; i < cur->polygon_count; ++i) associate(&cur->polygons[i], cur->drawn_at[i]);
    else
        for (i = 0; i < cur->polygon_count; ++i) {
            unsigned c;
            for (c = 0; c < cur->polygons[i].corners; ++c) cur->polygons[i].state[c] = FRAME_IDENTITY_NO_HISTORY;
        }
    memset(&f, 0, sizeof(f));
    f.number = s.frames; f.vblank = s.vblanks;
    f.first_projection = cur->first_projection; f.first_polygon = cur->first_polygon;
    f.display_start = s.display_start; f.tag = tag; f.flags = cur->flags;
    f.setup_count = (uint32_t)cur->setup_count; f.run_count = (uint32_t)cur->run_count;
    f.projection_count = (uint32_t)cur->projection_count; f.polygon_count = (uint32_t)cur->polygon_count;
    f.setups = cur->setups; f.runs = cur->runs; f.projections = cur->projections; f.polygons = cur->polygons;
    if (s.config.sink) s.config.sink(s.config.context, &f);
    ++s.frames;
    s.end_vblank = s.vblanks; s.have_end = 1; s.reset_pending = 0;
    cur->valid = 1;
    s.previous = cur; s.current = prev;
    begin(s.current);
}

static void display_start(void *context, uint32_t start) {
    (void)context;
    if (s.have_start && start != s.display_start) end_frame();
    s.display_start = start; s.have_start = 1;
}
