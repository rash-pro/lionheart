#include "frame_identity_host.h"
#include "frame_identity.h"
#include "ff8_ram_layout.h"
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
    uint8_t *buffer;
    size_t size, capacity;
    uint32_t *before;            /* the previous frame's SXY, for the jump measure */
    size_t before_count, before_capacity;
    uint64_t moves[2048];
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

/* FF8's mode word; the world map is excluded until it has a check of its own. */
static uint32_t classify(void *context, const cpu_t *c) {
    uint32_t mode;
    (void)context;
    if (!c) return 0;
    mode = (uint32_t)c->ram[FF8_MODE] | (uint32_t)c->ram[FF8_MODE + 1] << 8;
    return mode == 2 ? mode | FRAME_IDENTITY_EXCLUDE : mode;
}

static void put(const void *bytes, size_t n) {
    if (h.size + n > h.capacity) {
        size_t c = h.capacity ? h.capacity : 1 << 16;
        uint8_t *p;
        while (c < h.size + n) c *= 2;
        if (!(p = realloc(h.buffer, c))) { h.failed = 1; return; }
        h.buffer = p; h.capacity = c;
    }
    memcpy(h.buffer + h.size, bytes, n);
    h.size += n;
}
static void put32(uint32_t v) {
    uint8_t b[4] = {(uint8_t)v, (uint8_t)(v >> 8), (uint8_t)(v >> 16), (uint8_t)(v >> 24)};
    put(b, 4);
}
static void put64(uint64_t v) { put32((uint32_t)v); put32((uint32_t)(v >> 32)); }

/* The stream: every field the service delivers, little-endian, in the order
 * docs/frame-identity.md gives. Its digest is what native and interpreted
 * runs of one route must agree on. */
static void serialize(const frame_identity_frame_t *f) {
    uint32_t i, k;
    h.size = 0;
    put32(0x4d415246u); /* "FRAM" */
    put64(f->number); put64(f->vblank); put64(f->first_projection); put64(f->first_polygon);
    put32(f->display_start); put32(f->tag); put32(f->flags);
    put32(f->setup_count); put32(f->run_count); put32(f->projection_count); put32(f->polygon_count);
    for (i = 0; i < f->setup_count; ++i) {
        const frame_identity_setup_t *s = &f->setups[i];
        for (k = 0; k < 5; ++k) put32(s->rotation[k]);
        for (k = 0; k < 3; ++k) put32((uint32_t)s->translation[k]);
        put32((uint32_t)s->ofx); put32((uint32_t)s->ofy); put32(s->h); put32(s->shift); put32(s->lm);
    }
    for (i = 0; i < f->run_count; ++i) {
        const frame_identity_run_t *r = &f->runs[i];
        put32(r->first); put32(r->count); put32(r->setup); put32((uint32_t)r->previous);
        put32((uint32_t)r->match); put32(r->repeats); put64(r->key);
    }
    for (i = 0; i < f->projection_count; ++i) {
        const frame_identity_projection_t *p = &f->projections[i];
        put32((uint16_t)p->input[0] | (uint32_t)(uint16_t)p->input[1] << 16);
        put32((uint16_t)p->input[2] | (uint32_t)p->sz << 16);
        put32(p->sxy);
        for (k = 0; k < 3; ++k) put32((uint32_t)p->view[k]);
        put32(p->run); put32((uint32_t)p->previous); put32((uint32_t)p->match);
    }
    for (i = 0; i < f->polygon_count; ++i) {
        const frame_identity_polygon_t *q = &f->polygons[i];
        put32(q->opcode); put32(q->corners);
        for (k = 0; k < 4; ++k) {
            put32(q->xy[k]); put32((uint32_t)q->state[k] | (uint32_t)q->age[k] << 8); put32((uint32_t)q->projection[k]);
        }
    }
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

static void sink(void *context, const frame_identity_frame_t *f) {
    size_t i;
    (void)context;
    serialize(f);
    for (i = 0; i < h.size; ++i) h.digest = (h.digest ^ h.buffer[i]) * UINT64_C(0x100000001b3);
    if (h.dump && !h.failed && fwrite(h.buffer, 1, h.size, h.dump) != h.size) h.failed = 1;
    {
        uint64_t pairs;
        unsigned move = median_move(f, &pairs);
        uint32_t i;
        count(&h.total, f, move, pairs);
        count(&h.scene[scene_of(f->tag)], f, move, pairs);
        if (f->projection_count > h.before_capacity) {
            uint32_t *p = realloc(h.before, f->projection_count * sizeof(*p));
            if (p) { h.before = p; h.before_capacity = f->projection_count; }
        }
        h.before_count = f->projection_count <= h.before_capacity ? f->projection_count : 0;
        for (i = 0; i < h.before_count; ++i) h.before[i] = f->projections[i].sxy;
    }
#ifdef FF8_FRAME_ORACLE
    frame_oracle_judge(f);
#endif
}

static void bind(int on) {
    interp_gte_command = on ? frame_identity_gte_command : gte_cmd;
    native_modules_bind_projection(on ? frame_identity_rtps : NULL, on ? frame_identity_rtpt : NULL);
#ifdef FF8_FRAME_IDENTITY_LINKED
    linked_rtps = on ? frame_identity_rtps : gte_rtps;
    linked_rtpt = on ? frame_identity_rtpt : gte_rtpt;
#endif
}

int host_frame_identity_start(cpu_t *c, const char *dump, const char *oracle, int linked) {
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
    bind(1);
    h.running = 1;
    return 1;
}

void host_frame_identity_restored(cpu_t *c) {
    if (!h.running) return;
    frame_identity_rebind(c->hw->gpu);
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
}

int host_frame_identity_stop(void) {
    unsigned i;
    int ok = 1;
    if (!h.running) return 1;
    bind(0);
    frame_identity_stop();
    if (h.dump && fclose(h.dump)) h.failed = 1;
    if (h.failed) { fprintf(stderr, "Cannot write frame identity stream: %s\n", h.dump_path ? h.dump_path : ""); ok = 0; }
    report("all", &h.total);
    for (i = 0; i < SCENES; ++i) if (h.scene[i].frames) report(scene_names[i], &h.scene[i]);
    printf("Frame identity digest: %016" PRIx64 "\n", h.digest);
#ifdef FF8_FRAME_ORACLE
    if (!frame_oracle_stop()) ok = 0;
#endif
    free(h.buffer); free(h.dump_path); free(h.before);
    memset(&h, 0, sizeof(h));
    return ok;
}
