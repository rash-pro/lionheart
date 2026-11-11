#include "frame_identity_host.h"
#include "frame_identity.h"
#include "frame_record.h"
#include "ff8_ram_layout.h"
#include "gte.h"
#include "hw.h"
#include "interp.h"
#include "native_modules.h"
#ifdef FF8_FRAME_ORACLE
#include "frame_oracle.h"
#endif
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef FF8_FRAME_IDENTITY_LINKED
/* Linked recompiled code is compiled with gte_rtps and gte_rtpt renamed to
 * these, so that it can be observed the way modules are, through a binding. */
static void (*linked_rtps)(cpu_t *, unsigned, int) = gte_rtps;
static void (*linked_rtpt)(cpu_t *, unsigned, int) = gte_rtpt;
void frame_identity_linked_rtps(cpu_t *c, unsigned shift, int lm);
void frame_identity_linked_rtpt(cpu_t *c, unsigned shift, int lm);
void frame_identity_linked_rtps(cpu_t *c, unsigned shift, int lm) { linked_rtps(c, shift, lm); }
void frame_identity_linked_rtpt(cpu_t *c, unsigned shift, int lm) { linked_rtpt(c, shift, lm); }
#endif

/* Totals for one scene kind, from the game's mode word. */
typedef struct {
    uint64_t frames, reset, excluded, projections, paired, matched;
    uint64_t polygons, corners, corners_3d, linked, linked_polygons, held[8];
    uint64_t jumps, jump_pairs;
    /* The world map's check against where the game says its camera stood:
     * frames whose voted step is its change, is not, or that voted none; pairs
     * of points under a zero translation that it puts at one place in the
     * world, and at two. */
    uint64_t step_same, step_other, step_none, place_same, place_other;
} tally_t;

/* A frame whose paired vertices moved, at the median, at least this far on
 * screen is most likely a camera cut: right as identity, wrong to blend. */
enum { JUMP = 32 };

enum { SCENES = 5 };
static const char *const scene_names[SCENES] = {"field", "world", "battle", "menu", "other"};

static struct {
    int running;
    FILE *dump;
    char *dump_path;
    frame_record_bytes_t stream; /* the frame being delivered, as LHFRAME1 holds it */
    int recording;
    uint32_t *before;            /* the previous frame's SXY, for the jump measure */
    size_t before_count, before_capacity;
    uint64_t moves[2048];
    /* The world offset as each frame ended, and what the place check needs of
     * the frame before. */
    int32_t offset[3], before_offset[3];
    int offset_known, before_offset_known;
    uint64_t before_number;
    int16_t (*before_input)[3];
    uint8_t *before_zero;
    size_t before_input_capacity;
    int failed;
    uint64_t digest;
    tally_t total, scene[SCENES];
} h;

static unsigned scene_of(uint32_t tag) {
    switch (tag & 0xffffu) {
    case 1: return 0;
    case 2: return 1;
    case 3: return 2;
    case 6: return 3;
    default: return 4;
    }
}

/* FF8's mode word. The world map moves its terrain by the camera on the CPU
 * (FRAME_IDENTITY_POINTS); it is excluded until it has a check of its own. */
uint32_t host_frame_identity_classify(void *context, const cpu_t *c) {
    uint32_t mode;
    (void)context;
    if (!c) return 0;
    mode = (uint32_t)c->ram[FF8_MODE] | (uint32_t)c->ram[FF8_MODE + 1] << 8;
    return mode == 2 ? mode | FRAME_IDENTITY_POINTS | FRAME_IDENTITY_EXCLUDE : mode;
}

static int32_t word(const uint8_t *p) {
    return (int32_t)((uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24);
}

/* The same, noting where the camera stood by the game's own record: the world
 * offset plus the origin of its block. */
static uint32_t classify(void *context, const cpu_t *c) {
    uint32_t tag = host_frame_identity_classify(context, c);
    unsigned k;
    h.offset_known = c && (tag & 0xffffu) == 2;
    if (h.offset_known) {
        for (k = 0; k < 3; ++k) h.offset[k] = word(c->ram + FF8_WORLD_OFFSET + 4 * k);
        h.offset[0] += word(c->scratch + FF8_WORLD_ORIGIN);
        h.offset[2] += word(c->scratch + FF8_WORLD_ORIGIN + 4);
    }
    return tag;
}

/* The median screen move of paired vertices, in pixels (the larger axis). */
static unsigned median_move(const frame_identity_frame_t *f, uint64_t *pairs) {
    uint64_t n = 0, seen = 0;
    uint32_t i;
    unsigned d;
    memset(h.moves, 0, sizeof(h.moves));
    for (i = 0; i < f->projection_count; ++i) {
        int32_t p = f->projections[i].previous;
        uint32_t a, b;
        int dx, dy;
        if (p < 0 || (size_t)p >= h.before_count) continue;
        a = f->projections[i].sxy; b = h.before[p];
        dx = abs((int16_t)(a & 0xffff) - (int16_t)(b & 0xffff)); dy = abs((int16_t)(a >> 16) - (int16_t)(b >> 16));
        d = (unsigned)(dx > dy ? dx : dy);
        ++h.moves[d < 2047 ? d : 2047];
        ++n;
    }
    *pairs = n;
    if (!n) return 0;
    for (d = 0; d < 2048; ++d) if ((seen += h.moves[d]) * 2 >= n) return d;
    return 2047;
}

static void count(tally_t *t, const frame_identity_frame_t *f, unsigned move, uint64_t pairs) {
    uint32_t i, k;
    ++t->frames;
    if (pairs && move >= JUMP) { ++t->jumps; t->jump_pairs += pairs; }
    if (f->flags & FRAME_IDENTITY_RESET) ++t->reset;
    if (f->flags & FRAME_IDENTITY_EXCLUDED) ++t->excluded;
    t->projections += f->projection_count;
    for (i = 0; i < f->projection_count; ++i) {
        if (f->projections[i].previous >= 0) ++t->paired;
        if (f->projections[i].match >= 0) ++t->matched;
    }
    t->polygons += f->polygon_count;
    for (i = 0; i < f->polygon_count; ++i) {
        const frame_identity_polygon_t *q = &f->polygons[i];
        unsigned linked = 0, projected = 0;
        for (k = 0; k < q->corners; ++k) {
            ++t->held[q->state[k] & 7];
            if (q->state[k] == FRAME_IDENTITY_LINKED) ++linked;
            if (q->state[k] != FRAME_IDENTITY_NO_PROJECTION) ++projected;
        }
        t->corners += q->corners;
        t->linked += linked;
        if (projected) t->corners_3d += q->corners;
        if (linked == q->corners) t->linked_polygons += q->corners;
    }
}

static int zero_translation(const frame_identity_frame_t *f, uint32_t i) {
    const int32_t *t;
    if (f->projections[i].run >= f->run_count) return 0;
    t = f->setups[f->runs[f->projections[i].run].setup].translation;
    return !t[0] && !t[1] && !t[2];
}

/* The world map's check. The game's own offset says how far it moved every
 * point it moves by the camera, so two such points are one place in the world
 * when their inputs differ by the offset's change: the frame's voted step
 * should be that change, and each pair of such points should be one place.
 * The service reads none of this. Returns whether the change is known. */
static int place_check(const frame_identity_frame_t *f, int32_t change[3]) {
    tally_t *t[2];
    uint32_t i, k;
    int known = h.offset_known && h.before_offset_known && h.before_number + 1 == f->number &&
                !(f->flags & (FRAME_IDENTITY_RESET | FRAME_IDENTITY_OVERFLOW));
    t[0] = &h.total; t[1] = &h.scene[scene_of(f->tag)];
    for (k = 0; k < 3; ++k) change[k] = h.offset[k] - h.before_offset[k];
    if (known && (f->tag & FRAME_IDENTITY_POINTS)) {
        for (k = 0; k < 2; ++k) {
            if (!f->step_votes) ++t[k]->step_none;
            else if (!memcmp(f->step, change, sizeof(f->step))) ++t[k]->step_same;
            else ++t[k]->step_other;
        }
        for (i = 0; i < f->projection_count; ++i) {
            const frame_identity_projection_t *p = &f->projections[i];
            int same = 1;
            if (p->match < 0 || (size_t)p->match >= h.before_count || !h.before_zero[p->match] || !zero_translation(f, i))
                continue;
            for (k = 0; k < 3; ++k) if (p->input[k] - h.before_input[p->match][k] != change[k]) same = 0;
            for (k = 0; k < 2; ++k) ++*(same ? &t[k]->place_same : &t[k]->place_other);
        }
    }
    memcpy(h.before_offset, h.offset, sizeof(h.offset));
    h.before_offset_known = h.offset_known;
    h.before_number = f->number;
    return known;
}

static void sink(void *context, const frame_identity_frame_t *f) {
    size_t i;
    int32_t change[3];
    int known;
    (void)context;
    /* The stream: every field the service delivers, in the order
     * docs/frame-identity.md gives. Its digest is what native and interpreted
     * runs of one route must agree on. */
    h.stream.size = 0;
    frame_record_serialize(&h.stream, f);
    if (h.stream.failed) h.failed = 1;
    for (i = 0; i < h.stream.size; ++i) h.digest = (h.digest ^ h.stream.data[i]) * UINT64_C(0x100000001b3);
    if (h.dump && !h.failed && fwrite(h.stream.data, 1, h.stream.size, h.dump) != h.stream.size) h.failed = 1;
    if (h.recording) frame_record_frame(f);
    {
        uint64_t pairs;
        unsigned move = median_move(f, &pairs);
        uint32_t i;
        count(&h.total, f, move, pairs);
        count(&h.scene[scene_of(f->tag)], f, move, pairs);
        known = place_check(f, change);
        if (f->projection_count > h.before_capacity) {
            uint32_t *p = realloc(h.before, f->projection_count * sizeof(*p));
            int16_t (*in)[3] = realloc(h.before_input, f->projection_count * sizeof(*in));
            uint8_t *zero;
            if (p) h.before = p;
            if (in) h.before_input = in;
            zero = realloc(h.before_zero, f->projection_count);
            if (zero) h.before_zero = zero;
            if (p && in && zero) h.before_capacity = f->projection_count;
        }
        h.before_count = f->projection_count <= h.before_capacity ? f->projection_count : 0;
        for (i = 0; i < h.before_count; ++i) {
            h.before[i] = f->projections[i].sxy;
            memcpy(h.before_input[i], f->projections[i].input, sizeof(h.before_input[i]));
            h.before_zero[i] = (uint8_t)zero_translation(f, (uint32_t)i);
        }
    }
#ifdef FF8_FRAME_ORACLE
    frame_oracle_world(known && (f->tag & FRAME_IDENTITY_POINTS), change);
    frame_oracle_judge(f);
#else
    (void)known;
#endif
}

void host_frame_identity_bind(int on) {
    interp_gte_command = on ? frame_identity_gte_command : gte_cmd;
    native_modules_bind_projection(on ? frame_identity_rtps : NULL, on ? frame_identity_rtpt : NULL);
#ifdef FF8_FRAME_IDENTITY_LINKED
    linked_rtps = on ? frame_identity_rtps : gte_rtps;
    linked_rtpt = on ? frame_identity_rtpt : gte_rtpt;
#endif
}

int host_frame_identity_start(cpu_t *c, const char *dump, const char *oracle, int linked,
                              const char *record, uint64_t from, uint64_t until) {
    frame_identity_config_t config;
#ifndef FF8_FRAME_IDENTITY_LINKED
    if (linked) {
        fprintf(stderr, "Frame identity cannot observe recompiled code linked into this host; "
                        "configure it with -DFF8_FRAME_IDENTITY_LINKED=ON or run with --interpreter\n");
        return 0;
    }
#else
    (void)linked;
#endif
#ifdef FF8_FRAME_ORACLE
    if (oracle && !frame_oracle_start(c, oracle)) return 0;
#else
    if (oracle) { fprintf(stderr, "--frame-oracle needs a developer build with -DFF8_FRAME_ORACLE=ON\n"); return 0; }
#endif
    memset(&h, 0, sizeof(h));
    h.digest = UINT64_C(0xcbf29ce484222325);
    if (dump) {
        static const char magic[8] = {'L', 'H', 'F', 'R', 'A', 'M', 'E', '1'};
        if (!(h.dump = fopen(dump, "wb")) || fwrite(magic, 1, 8, h.dump) != 8) {
            fprintf(stderr, "Cannot write frame identity stream: %s\n", dump);
            if (h.dump) fclose(h.dump);
            h.dump = NULL;
            return 0;
        }
        h.dump_path = malloc(strlen(dump) + 1);
        if (h.dump_path) strcpy(h.dump_path, dump);
    }
    memset(&config, 0, sizeof(config));
    config.sink = sink; config.classify = classify;
    if (!frame_identity_start(c->hw->gpu, &config)) { fprintf(stderr, "Cannot start frame identity\n"); return 0; }
    if (record) {
        if (!frame_record_start(c->hw->gpu, record, from, until)) {
            fprintf(stderr, "Cannot record frames to %s (a build with save states is needed)\n", record);
            frame_identity_stop();
            return 0;
        }
        h.recording = 1;
    }
    host_frame_identity_bind(1);
    h.running = 1;
    return 1;
}

void host_frame_identity_restored(cpu_t *c) {
    if (!h.running) return;
    frame_identity_rebind(c->hw->gpu);
    if (h.recording) frame_record_restored(c->hw->gpu);
#ifdef FF8_FRAME_ORACLE
    frame_oracle_restored(c);
#endif
}

static double percent(uint64_t a, uint64_t b) { return b ? 100.0 * (double)a / (double)b : 0.0; }

static void report(const char *label, const tally_t *t) {
    printf("Frame identity %s: frames=%" PRIu64 " reset=%" PRIu64 " excluded=%" PRIu64 " projections=%" PRIu64
           " paired=%" PRIu64 " (%.2f%%) matched=%" PRIu64 " (%.2f%%) polygons=%" PRIu64 " corners=%" PRIu64
           " corners3d=%" PRIu64 " linked=%" PRIu64 " (%.2f%% of 3D) in-linked-polygons=%" PRIu64 " (%.2f%% of 3D)"
           " none=%" PRIu64 " split=%" PRIu64 " unpaired=%" PRIu64 " ambiguous=%" PRIu64 " no-history=%" PRIu64
           " excluded-corners=%" PRIu64 " jumps=%" PRIu64 " jump-pairs=%" PRIu64 "\n",
           label, t->frames, t->reset, t->excluded, t->projections, t->paired, percent(t->paired, t->projections),
           t->matched, percent(t->matched, t->projections), t->polygons, t->corners, t->corners_3d, t->linked,
           percent(t->linked, t->corners_3d), t->linked_polygons, percent(t->linked_polygons, t->corners_3d),
           t->held[FRAME_IDENTITY_NO_PROJECTION], t->held[FRAME_IDENTITY_SPLIT], t->held[FRAME_IDENTITY_UNPAIRED],
           t->held[FRAME_IDENTITY_AMBIGUOUS], t->held[FRAME_IDENTITY_NO_HISTORY], t->held[FRAME_IDENTITY_EXCLUDED_CORNER],
           t->jumps, t->jump_pairs);
    if (t->step_same || t->step_other || t->step_none)
        printf("Frame identity %s points: step-same=%" PRIu64 " step-other=%" PRIu64 " step-none=%" PRIu64
               " place-same=%" PRIu64 " place-other=%" PRIu64 "\n",
               label, t->step_same, t->step_other, t->step_none, t->place_same, t->place_other);
}

int host_frame_identity_stop(void) {
    unsigned i;
    int ok = 1;
    if (!h.running) return 1;
    host_frame_identity_bind(0);
    if (h.recording && !frame_record_stop()) { fprintf(stderr, "Cannot write the frame recording\n"); ok = 0; }
    frame_identity_stop();
    if (h.dump && fclose(h.dump)) h.failed = 1;
    if (h.failed) { fprintf(stderr, "Cannot write frame identity stream: %s\n", h.dump_path ? h.dump_path : ""); ok = 0; }
    report("all", &h.total);
    for (i = 0; i < SCENES; ++i) if (h.scene[i].frames) report(scene_names[i], &h.scene[i]);
    printf("Frame identity digest: %016" PRIx64 "\n", h.digest);
#ifdef FF8_FRAME_ORACLE
    if (!frame_oracle_stop()) ok = 0;
#endif
    frame_record_bytes_free(&h.stream); free(h.dump_path); free(h.before); free(h.before_input); free(h.before_zero);
    memset(&h, 0, sizeof(h));
    return ok;
}
