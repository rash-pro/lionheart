#if defined(FF8_PGXP)
/* Original screen-coordinate cache, separate from guest memory and timing. */
#include "pgxp.h"
#include "cpu.h"
#include "hw.h"
#include <string.h>

enum { SLOTS = 32768, PROBES = 32, PGXP_MAX_CANDS = 4 };
typedef struct {
    int64_t x, y;
    uint32_t z;
} pgxp_cand_t;
typedef struct {
    int64_t x, y;
    uint32_t key, z;
    unsigned used:1, ambiguous:1, valid:1, depth_conflict:1;
    uint8_t cand_count, cand_next;
    pgxp_cand_t cands[PGXP_MAX_CANDS];
} pgxp_entry_t;
static pgxp_entry_t cache[PGXP_GENERATIONS][SLOTS];
static struct gpu *owner;
static pgxp_mode_t mode;
static pgxp_policy_t policy;
typedef struct {
    uint32_t key;
    unsigned used, valid, known, ambiguous, depth_conflict, age;
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
        if (valid && e->z > 0 && ((uint64_t)z * 2u > (uint64_t)e->z * 3u || (uint64_t)e->z * 2u > (uint64_t)z * 3u)) {
            e->depth_conflict = 1;
        }
    }
    e->x = x; e->y = y; e->z = z; e->valid = (unsigned)valid;
    if (valid) {
        unsigned ci, found = 0;
        for (ci = 0; ci < e->cand_count; ++ci) {
            if (e->cands[ci].x == x && e->cands[ci].y == y && e->cands[ci].z == z) {
                found = 1;
                break;
            }
            if ((uint64_t)z * 2u > (uint64_t)e->cands[ci].z * 3u || (uint64_t)e->cands[ci].z * 2u > (uint64_t)z * 3u) {
                e->depth_conflict = 1;
            }
        }
        if (!found) {
            unsigned dst;
            if (e->cand_count < PGXP_MAX_CANDS) {
                dst = e->cand_count++;
            } else {
                dst = e->cand_next++ % PGXP_MAX_CANDS;
                e->depth_conflict = 1;
            }
            e->cands[dst].x = x;
            e->cands[dst].y = y;
            e->cands[dst].z = z;
        }
    }
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
        if (e->depth_conflict || (latest->valid && e->valid &&
            ((uint64_t)latest->z * 2u > (uint64_t)e->z * 3u || (uint64_t)e->z * 2u > (uint64_t)latest->z * 3u))) {
            result.depth_conflict = 1;
        }
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
static pgxp_vertex_t make_vertex(int ix, int iy, int64_t x, int64_t y, uint32_t z) {
    pgxp_vertex_t v;
    double dx = (double)(x - (int64_t)ix * 65536) / 65536.0 - 0.5;
    double dy = (double)(y - (int64_t)iy * 65536) / 65536.0 - 0.5;
    if (dx < -0.5) dx = -0.5;
    if (dx > 0.5) dx = 0.5;
    if (dy < -0.5) dy = -0.5;
    if (dy > 0.5) dy = 0.5;
    v.x = (float)ix + (float)dx;
    v.y = (float)iy + (float)dy;
    v.w = (float)z;
    return v;
}

static double depth_ratio(double a, double b) {
    if (a <= 0.0 || b <= 0.0) return 1e9;
    return a >= b ? (a / b) : (b / a);
}

/* Finds the depth-consistent PGXP candidate for an ambiguous pixel key across active
 * generation banks when a foreground object and background terrain share an integer pixel. */
static int find_depth_candidate(uint32_t key, double ref_z, double max_ratio, pgxp_vertex_t *out) {
    int ix = (int)(key & 32767) - (int)(key & 32768);
    int iy = (int)((key >> 16) & 32767) - (int)((key >> 16) & 32768);
    double best_ratio = max_ratio;
    int found = 0;
    unsigned age;
    for (age = 0; age < PGXP_GENERATIONS; ++age) {
        unsigned bank = (epoch + PGXP_GENERATIONS - age) % PGXP_GENERATIONS;
        const pgxp_entry_t *e = find(bank, key, 0);
        unsigned ci;
        if (!e) continue;
        for (ci = 0; ci < e->cand_count; ++ci) {
            double r = depth_ratio((double)e->cands[ci].z, ref_z);
            if (r < best_ratio) {
                best_ratio = r;
                *out = make_vertex(ix, iy, e->cands[ci].x, e->cands[ci].y, e->cands[ci].z);
                found = 1;
            }
        }
        /* Stop once we have a tight match from the newest bank containing this vertex. */
        if (found && best_ratio < 1.25) break;
    }
    return found;
}

/* Resolves depth-ambiguous vertices on a polygon against its depth-unambiguous corners so
 * overlapping foreground/background vertices never steal each other's Z or subpixel XY. */
static int disambiguate_polygon(const uint32_t *keys, const resolution_t *res,
                                pgxp_vertex_t *verts, unsigned n, int ox, int oy,
                                unsigned hits) {
    unsigned i, ref_count = 0, any_depth_conflict = 0;
    double ref_z_sum = 0.0, ref_min_z = 1e9, ref_max_z = 0.0;
    int bad_perspective = 0;
    if (policy != PGXP_LATEST) return 0;

    for (i = 0; i < n; ++i) {
        if (res[i].depth_conflict) any_depth_conflict = 1;
        if (res[i].valid && !res[i].depth_conflict && verts[i].w > 0.0f) {
            double wz = (double)verts[i].w;
            ref_z_sum += wz;
            if (wz < ref_min_z) ref_min_z = wz;
            if (wz > ref_max_z) ref_max_z = wz;
            ++ref_count;
        }
    }
    if (!any_depth_conflict) return 0;

    if (ref_count > 0) {
        double ref_z = ref_z_sum / (double)ref_count;
        for (i = 0; i < n; ++i) {
            double wz, cur_r;
            pgxp_vertex_t cand;
            if (!res[i].valid || !res[i].depth_conflict) continue;
            wz = (double)verts[i].w;
            cur_r = wz < ref_min_z ? (ref_min_z / wz) : (wz > ref_max_z ? (wz / ref_max_z) : 1.0);
            if (cur_r <= 1.6) continue;
            if (find_depth_candidate(keys[i], ref_z, 2.2, &cand) &&
                depth_ratio((double)cand.w, ref_z) < cur_r * 0.75) {
                verts[i].x = cand.x + (float)ox;
                verts[i].y = cand.y + (float)oy;
                verts[i].w = cand.w;
            } else if (cur_r > 2.2) {
                /* No depth-consistent candidate survived: drop stolen subpixel/depth. */
                int ix = (int)(keys[i] & 32767) - (int)(keys[i] & 32768);
                int iy = (int)((keys[i] >> 16) & 32767) - (int)((keys[i] >> 16) & 32768);
                verts[i].x = (float)(ix + ox);
                verts[i].y = (float)(iy + oy);
                verts[i].w = 1.0f;
                bad_perspective = 1;
            }
        }
    } else if (hits == n) {
        /* All corners had depth conflicts: check if their current depths diverge wildly
         * and search for a mutually depth-consistent candidate assignment. */
        double wmin = verts[0].w, wmax = verts[0].w;
        for (i = 1; i < n; ++i) {
            if (verts[i].w < wmin) wmin = verts[i].w;
            if (verts[i].w > wmax) wmax = verts[i].w;
        }
        if (depth_ratio(wmax, wmin) > 2.2) {
            double best_spread = 2.2;
            unsigned best_matches = 0;
            pgxp_vertex_t best_verts[4];
            int found_consistent = 0;
            unsigned age;
            for (age = 0; age < PGXP_GENERATIONS; ++age) {
                unsigned bank = (epoch + PGXP_GENERATIONS - age) % PGXP_GENERATIONS;
                const pgxp_entry_t *e0 = find(bank, keys[0], 0);
                unsigned ci;
                if (!e0) continue;
                for (ci = 0; ci < e0->cand_count; ++ci) {
                    double seed_z = (double)e0->cands[ci].z;
                    pgxp_vertex_t trial[4];
                    int ix0 = (int)(keys[0] & 32767) - (int)(keys[0] & 32768);
                    int iy0 = (int)((keys[0] >> 16) & 32767) - (int)((keys[0] >> 16) & 32768);
                    double tmin = seed_z, tmax = seed_z;
                    unsigned j, matches = (depth_ratio(seed_z, (double)verts[0].w) < 1.25) ? 1u : 0u;
                    int ok = 1;
                    trial[0] = make_vertex(ix0, iy0, e0->cands[ci].x, e0->cands[ci].y, e0->cands[ci].z);
                    trial[0].x += (float)ox; trial[0].y += (float)oy;
                    for (j = 1; j < n; ++j) {
                        pgxp_vertex_t cj;
                        if (!find_depth_candidate(keys[j], seed_z, 2.2, &cj)) { ok = 0; break; }
                        if (cj.w < tmin) tmin = cj.w;
                        if (cj.w > tmax) tmax = cj.w;
                        if (depth_ratio((double)cj.w, (double)verts[j].w) < 1.25) ++matches;
                        cj.x += (float)ox; cj.y += (float)oy;
                        trial[j] = cj;
                    }
                    if (ok) {
                        double sp = depth_ratio(tmax, tmin);
                        if (sp < 2.2 && (!found_consistent || matches > best_matches ||
                                         (matches == best_matches && sp < best_spread))) {
                            best_matches = matches;
                            best_spread = sp;
                            memcpy(best_verts, trial, n * sizeof(pgxp_vertex_t));
                            found_consistent = 1;
                        }
                    }
                }
            }
            if (found_consistent) {
                memcpy(verts, best_verts, n * sizeof(pgxp_vertex_t));
            } else {
                bad_perspective = 1;
            }
        }
    }

    return bad_perspective;
}

void pgxp_polygon(struct gpu *g, const gpu_vertex_t *v, unsigned n, int ox, int oy) {
    unsigned i, hits = 0, any = 0, conflict = 0, oldest = 0;
    int bad_persp = 0;
    uint32_t keys[4];
    resolution_t res[4];
    if (g != owner || mode == PGXP_OFF) return;
    precise = 0; first = 0; ++stats.polygons;
    for (i = 0; i < n; ++i) {
        keys[i] = ((uint32_t)(v[i].x - ox) & 65535) | ((uint32_t)(v[i].y - oy) << 16);
        res[i] = resolve(keys[i]);
        polygon[i] = res[i].vertex; polygon[i].x += ox; polygon[i].y += oy;
        any |= res[i].known; conflict |= res[i].ambiguous; hits += res[i].valid;
        if (res[i].age > oldest) oldest = res[i].age;
        ++stats.vertex_lookups; stats.vertex_precise += res[i].valid;
        stats.vertex_ambiguous += res[i].ambiguous;
    }
    if (any) ++stats.candidates;
    if (conflict) ++stats.ambiguous;
    if (hits == n) { ++stats.precise; ++stats.precise_age[oldest]; }
    else if (hits) ++stats.partial;
    else if (any && !conflict) ++stats.misses;
    if (conflict) bad_persp = disambiguate_polygon(keys, res, polygon, n, ox, oy, hits);
    /* Geometry is resolved per key for the whole frame. Missing depth disables
     * perspective on this polygon, without changing any endpoint position. */
    if (mode != PGXP_PERSPECTIVE || hits != n || bad_persp)
        for (i = 0; i < n; ++i) polygon[i].w = 1;
    precise = hits != 0;
}
unsigned pgxp_packet(struct gpu *g, const uint32_t *packet, pgxp_vertex_t out[4]) {
    unsigned op = packet[0] >> 24, n = (op & 8) ? 4 : 3, i, at = 1, mask = 0, hits = 0, conflict = 0;
    int bad_persp = 0;
    uint32_t keys[4];
    resolution_t res[4];
    if (g != owner || mode == PGXP_OFF || op < 0x20 || op >= 0x40) return 0;
    for (i = 0; i < n; ++i) {
        uint32_t xy;
        if (i && (op & 16)) ++at;
        xy = packet[at++];
        if (op & 4) ++at;
        /* The GPU's 11-bit coordinates, as pgxp_polygon keys them. */
        keys[i] = (uint32_t)(((int)(xy & 1023) - (int)(xy & 1024)) & 65535) |
                  (uint32_t)(((int)((xy >> 16) & 1023) - (int)((xy >> 16) & 1024)) & 65535) << 16;
        res[i] = resolve(keys[i]);
        out[i] = res[i].vertex;
        conflict |= res[i].ambiguous;
        if (res[i].valid) { mask |= 1u << i; ++hits; }
    }
    if (conflict) bad_persp = disambiguate_polygon(keys, res, out, n, 0, 0, hits);
    for (i = 0; i < n; ++i) {
        if (mode != PGXP_PERSPECTIVE || !res[i].valid || bad_persp) out[i].w = 0;
    }
    return mask;
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
