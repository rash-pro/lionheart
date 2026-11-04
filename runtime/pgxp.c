#if defined(FF8_PGXP)
/* Original screen-coordinate cache, separate from guest memory and timing. */
#include "pgxp.h"
#include "cpu.h"
#include "hw.h"
#include <string.h>

enum { SLOTS = 32768, PROBES = 32 };
typedef struct {
    int64_t x, y;
    uint32_t key, z;
    unsigned used:1, ambiguous:1, valid:1;
} pgxp_entry_t;
static pgxp_entry_t cache[PGXP_GENERATIONS][SLOTS];
static struct gpu *owner;
static pgxp_mode_t mode;
static pgxp_policy_t policy;
typedef struct {
    uint32_t key;
    unsigned used, valid, known, ambiguous, age;
    pgxp_vertex_t vertex;
} resolution_t;
static resolution_t resolved[SLOTS];
static uint64_t frame;
static pgxp_resolved_sink_t resolved_sink;
static void *resolved_context;
static unsigned epoch, first, precise;
static pgxp_vertex_t polygon[4];
static pgxp_stats_t stats;
static pgxp_sink_t sink;
static void *sink_context;

static pgxp_entry_t *find(unsigned bank, uint32_t key, int insert) {
    uint32_t hash = key;
    unsigned i;
    hash ^= hash >> 16; hash *= 0x7feb352du; hash ^= hash >> 15;
    for (i = 0; i < PROBES; ++i) {
        pgxp_entry_t *e = &cache[bank][(hash + i) & (SLOTS - 1)];
        if (e->used && e->key == key) return e;
        if (!e->used) {
            if (!insert) return NULL;
            e->used = 1; e->key = key;
            return e;
        }
    }
    return NULL;
}
void pgxp_bind(struct gpu *g, pgxp_mode_t value) {
    owner = g; mode = value; epoch = first = precise = 0; frame = 0; policy = PGXP_LATEST;
    memset(resolved, 0, sizeof(resolved)); resolved_sink = NULL; resolved_context = NULL;
    memset(cache, 0, sizeof(cache)); memset(&stats, 0, sizeof(stats));
    sink = NULL; sink_context = NULL;
}
void pgxp_policy(pgxp_policy_t value) { policy = value; }
void pgxp_resolved_sink(pgxp_resolved_sink_t value, void *context) { resolved_sink = value; resolved_context = context; }
void pgxp_record_sink(pgxp_sink_t value, void *context) { sink = value; sink_context = context; }
void pgxp_record(const struct cpu *c, uint32_t key, int64_t x, int64_t y, uint32_t z) {
    pgxp_entry_t *e;
    int ix = (int)(key & 32767) - (int)(key & 32768);
    int iy = (int)((key >> 16) & 32767) - (int)((key >> 16) & 32768);
    int valid;
    if (!owner || (mode == PGXP_OFF && !sink)) return;
    if (c && (!c->hw || c->hw->gpu != owner)) return;
    if (sink) sink(sink_context, c, key, x, y, z);
    ++stats.projections;
    /* Clipped XY and invalid depth cannot provide a reliable projection. */
    valid = z > 0 && z <= 65535 && ix > -1024 && ix < 1023 && iy > -1024 && iy < 1023 &&
        x >= (int64_t)ix * 65536 && x < ((int64_t)ix + 1) * 65536 &&
        y >= (int64_t)iy * 65536 && y < ((int64_t)iy + 1) * 65536;
    e = find(epoch, key, 1);
    if (!e) { ++stats.overflow; return; }
    if (!valid) { ++stats.rejected; e->ambiguous = 1; }
    if (e->valid && (e->x != x || e->y != y || e->z != z)) {
        if (!e->ambiguous) ++stats.collisions;
        e->ambiguous = 1;
    }
    e->x = x; e->y = y; e->z = z; e->valid = (unsigned)valid;
}
void pgxp_reset(struct gpu *g) {
    if (g != owner || (mode == PGXP_OFF && !sink)) return;
    memset(cache, 0, sizeof(cache)); memset(resolved, 0, sizeof(resolved)); precise = 0;
}
void pgxp_vblank(struct gpu *g) {
    if (g != owner || (mode == PGXP_OFF && !sink)) return;
    ++frame; memset(resolved, 0, sizeof(resolved));
    epoch = (epoch + 1) % PGXP_GENERATIONS;
    memset(cache[epoch], 0, sizeof(cache[epoch])); precise = 0;
}
static int same(const pgxp_entry_t *a, const pgxp_entry_t *b) {
    return a->x == b->x && a->y == b->y && a->z == b->z;
}
int pgxp_mark_ambiguous(uint32_t key) {
    pgxp_entry_t *e;
    if (!owner || (mode == PGXP_OFF && !sink)) return 1;
    e = find(epoch, key, 0);
    if (!e) return 0;
    e->ambiguous = 1; return 1;
}
static resolution_t resolve(uint32_t key) {
    uint32_t hash = key;
    resolution_t result, *slot = NULL;
    const pgxp_entry_t *latest = NULL;
    unsigned i, age;
    int ix = (int)(key & 32767) - (int)(key & 32768);
    int iy = (int)((key >> 16) & 32767) - (int)((key >> 16) & 32768);
    hash ^= hash >> 16; hash *= 0x7feb352du; hash ^= hash >> 15;
    for (i = 0; i < PROBES; ++i) {
        resolution_t *r = &resolved[(hash + i) & (SLOTS - 1)];
        if (r->used && r->key == key) return *r;
        if (!r->used) { slot = r; break; }
    }
    memset(&result, 0, sizeof(result)); result.used = 1; result.key = key;
    result.vertex.x = (float)ix; result.vertex.y = (float)iy; result.vertex.w = 1;
    for (age = 0; age < PGXP_GENERATIONS; ++age) {
        unsigned bank = (epoch + PGXP_GENERATIONS - age) % PGXP_GENERATIONS;
        const pgxp_entry_t *e = find(bank, key, 0);
        if (!e) continue;
        result.known = 1;
        if (!latest) { latest = e; result.age = age; }
        if (e->ambiguous || !e->valid || !same(latest, e)) result.ambiguous = 1;
    }
    if (slot && latest && latest->valid && (policy == PGXP_LATEST || !result.ambiguous)) {
        /* PS1 stores the integer sample cell. Center its available Q16 residual
         * on that sample, matching Vulkan's scaled pixel-center convention. */
        double dx = (double)(latest->x - (int64_t)ix * 65536) / 65536.0 - 0.5;
        double dy = (double)(latest->y - (int64_t)iy * 65536) / 65536.0 - 0.5;
        if (dx < -0.5) dx = -0.5;
        if (dx > 0.5) dx = 0.5;
        if (dy < -0.5) dy = -0.5;
        if (dy > 0.5) dy = 0.5;
        result.vertex.x += (float)dx; result.vertex.y += (float)dy;
        result.vertex.w = (float)latest->z; result.valid = 1;
    }
    if (slot) *slot = result; else ++stats.overflow;
    if (resolved_sink) resolved_sink(resolved_context, frame, key, result.vertex,
                                     result.valid, result.ambiguous, result.age);
    return result;
}
void pgxp_polygon(struct gpu *g, const gpu_vertex_t *v, unsigned n, int ox, int oy) {
    unsigned i, hits = 0, any = 0, conflict = 0, oldest = 0;
    if (g != owner || mode == PGXP_OFF) return;
    precise = 0; first = 0; ++stats.polygons;
    for (i = 0; i < n; ++i) {
        uint32_t key = ((uint32_t)(v[i].x - ox) & 65535) | ((uint32_t)(v[i].y - oy) << 16);
        resolution_t r = resolve(key);
        polygon[i] = r.vertex; polygon[i].x += ox; polygon[i].y += oy;
        any |= r.known; conflict |= r.ambiguous; hits += r.valid;
        if (r.age > oldest) oldest = r.age;
        ++stats.vertex_lookups; stats.vertex_precise += r.valid;
        stats.vertex_ambiguous += r.ambiguous;
    }
    if (any) ++stats.candidates;
    if (conflict) ++stats.ambiguous;
    if (hits == n) { ++stats.precise; ++stats.precise_age[oldest]; }
    else if (hits) ++stats.partial;
    else if (any && !conflict) ++stats.misses;
    /* Geometry is resolved per key for the whole frame. Missing depth disables
     * perspective on this polygon, without changing any endpoint position. */
    if (mode != PGXP_PERSPECTIVE || hits != n)
        for (i = 0; i < n; ++i) polygon[i].w = 1;
    precise = hits != 0;
}
void pgxp_select(struct gpu *g, unsigned value) { if (g == owner) first = value; }
int pgxp_triangle(const gpu_raster_t *raster, pgxp_vertex_t out[3]) {
    if (!owner || mode == PGXP_OFF || !precise || gpu_raster_state(owner) != raster) return 0;
    memcpy(out, polygon + first, 3 * sizeof(*out)); return 1;
}
pgxp_stats_t pgxp_stats(void) { return stats; }
void pgxp_visit(unsigned age, pgxp_sink_t visit, pgxp_mark_sink_t mark, void *context) {
    unsigned i, bank = (epoch + PGXP_GENERATIONS - age) % PGXP_GENERATIONS;
    for (i = 0; i < SLOTS; ++i) {
        const pgxp_entry_t *e = &cache[bank][i];
        if (e->used) {
            visit(context, NULL, e->key, e->x, e->y, e->valid ? e->z : 0);
            if (e->valid && e->ambiguous && mark) mark(context, e->key);
        }
    }
}
#else
typedef int ff8_pgxp_disabled_translation_unit;
#endif
