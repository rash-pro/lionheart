#include "frame_tags.h"
#include "cpu.h"
#include "hw.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if defined(FF8_FRAME_PROVENANCE)

/* Raw leaves carry a word address and a write generation, including writes of
 * identical bits. Copies carry their source leaf; destination write generations
 * still advance. Byte lanes retain partial stores and unaligned copy chains.
 * Composite leaves are interned by exact operands (hash collisions are checked).
 * Only projection + a different projection is mixed; scalar arithmetic retains
 * a projection. No lookup by packed SXY, material, UV or packet position occurs.
 */
#define TAG_KIND (UINT64_C(3) << 62)
#define TAG_EXPR (UINT64_C(1) << 62)
#define TAG_PROJECT (UINT64_C(2) << 62)
#define TAG_ADDRESS_BITS 22
#define TAG_ADDRESS_MASK ((1u << TAG_ADDRESS_BITS) - 1)
#define EXPR_COUNT (1u << 18)
#define TAG_LITERAL (UINT64_MAX - 2)
#define PROJECT_COUNT (1u << 18)
typedef struct { frame_tag_t a, b, minimum; unsigned split; } expression_t;
typedef struct {
    frame_tag_t vertex, context;
    uint64_t serial;
    uint32_t pc, xy, depth, frame, occurrence;
    uint32_t input_xy, input_z, matrix[8];
    int64_t x, y;
} projection_t;
struct frame_tags {
    frame_register_tags_t cpu;
    frame_tag_t data[32], control[32], port, pipeline[256], pipeline_location[256];
    frame_tag_t *memory, *locations, *lifetimes;
    frame_tag_t control_locations[32];
    uint64_t reload_generation;
    uint64_t movie_words, movie_until;
    uint64_t generation, initial_generation, expression_serial, projection_serial;
    expression_t *expressions;
    uint32_t *expression_hash;
    unsigned node_capacity;
    unsigned expression_live, free_expression, collect_after;
    unsigned gc_holds;
    uint64_t collections, reclaimed;
    projection_t *projections;
    frame_corner_t polygon[4];
    unsigned frame, pass, display, trace_first, expression_capacity;
    uint64_t corners[4], exhausted;
    struct { frame_tag_t context; unsigned pass, count; } instances[4096];
    uint64_t matrix_serial, projected_matrix;
    frame_tag_t projected_context;
    unsigned projected_pass, occurrence;
    FILE *trace;
};

static unsigned offset(uint32_t address) {
    uint32_t p = address & 0x1fffffffu;
    if (p < 0x800000u) return p & (RAM_SIZE - 1);
    if (p - 0x1f800000u < 1024) return RAM_SIZE + p - 0x1f800000u;
    return UINT32_MAX;
}

int frame_tags_enable(cpu_t *c) {
    struct frame_tags *s;
    const char *path;
    if (c->frame_tags) return 1;
    s = calloc(1, sizeof(*s));
    if (!s) return 0;
    s->memory = calloc(RAM_SIZE + 1024, sizeof(*s->memory));
    s->locations = calloc((RAM_SIZE + 1024) / 4, sizeof(*s->locations));
    s->lifetimes = calloc((RAM_SIZE + 1024) / 4, sizeof(*s->lifetimes));
    s->reload_generation = 1;
    s->movie_words = c->hw ? c->hw->mdec.words_out : 0;
    s->expression_capacity = EXPR_COUNT;
    s->node_capacity = EXPR_COUNT;
    s->collect_after = EXPR_COUNT * 2;
    s->expressions = calloc(s->node_capacity, sizeof(*s->expressions));
    s->expression_hash = calloc(s->expression_capacity, sizeof(*s->expression_hash));
    s->projections = calloc(PROJECT_COUNT, sizeof(*s->projections));
    c->frame_tags = s;
    if (!s->memory || !s->locations || !s->lifetimes || !s->expressions || !s->expression_hash || !s->projections) { frame_tags_destroy(c); return 0; }
    s->initial_generation = s->generation = 1;
    path = getenv("FF8_PROVENANCE_TRACE");
    if (path && *path) {
        s->trace = fopen(path, "w");
        if (!s->trace) { frame_tags_destroy(c); return 0; }
        setvbuf(s->trace, NULL, _IOFBF, 1024 * 1024);
        fputs("kind,frame,pass,corner,opcode,tag,vertex,context,pc,x,y,depth,source_xy,source_z,occurrence\n", s->trace);
    }
    path = getenv("FF8_PROVENANCE_FIRST");
    s->trace_first = path ? (unsigned)strtoul(path, NULL, 0) : 0;
    return 1;
}

void frame_tags_destroy(cpu_t *c) {
    struct frame_tags *s = c->frame_tags ? c->frame_tags : c->frame_tags_paused;
    if (!s) return;
    if (s->trace) fclose(s->trace);
    fprintf(stderr, "Frame provenance: corners=%llu/%llu/%llu/%llu expressions=%llu live=%u collections=%llu reclaimed=%llu exhausted=%llu generations=%llu\n",
        (unsigned long long)s->corners[0], (unsigned long long)s->corners[1],
        (unsigned long long)s->corners[2], (unsigned long long)s->corners[3],
        (unsigned long long)s->expression_serial, s->expression_live,
        (unsigned long long)s->collections, (unsigned long long)s->reclaimed,
        (unsigned long long)s->exhausted, (unsigned long long)s->generation);
    free(s->memory); free(s->locations); free(s->lifetimes); free(s->expressions); free(s->expression_hash); free(s->projections); free(s);
    c->frame_tags = c->frame_tags_paused = NULL;
}

void frame_tags_pause(cpu_t *c, int paused) {
    if (paused && c->frame_tags) {
        c->frame_tags_paused = c->frame_tags;
        c->frame_tags = NULL;
    } else if (!paused && c->frame_tags_paused) {
        c->frame_tags = c->frame_tags_paused;
        c->frame_tags_paused = NULL;
        frame_tags_reset(c);
    }
}

void frame_tags_present(cpu_t *c) {
    struct frame_tags *s = c->frame_tags ? c->frame_tags : c->frame_tags_paused;
    uint64_t frame;
    if (!s || !c->hw) return;
    frame = c->hw->vblanks;
    /* Movie pixels can be either packed RGB24 or RGB15. Retain the exclusion
     * across decoder gaps; a new geometry history is required after either. */
    if (s->movie_words != c->hw->mdec.words_out) s->movie_until = frame + 8;
    s->movie_words = c->hw->mdec.words_out;
    frame_tags_pause(c, frame < s->movie_until || !!(gpu_status(c->hw->gpu) & (1u << 21)));
}

void frame_tags_reset(cpu_t *c) {
    struct frame_tags *s = c->frame_tags;
    unsigned i;
    if (!s) return;
    memset(&s->cpu, 0, sizeof(s->cpu));
    memset(s->data, 0, sizeof(s->data)); memset(s->control, 0, sizeof(s->control));
    memset(s->memory, 0, (RAM_SIZE + 1024) * sizeof(*s->memory));
    memset(s->locations, 0, (RAM_SIZE + 1024) / 4 * sizeof(*s->locations));
    memset(s->lifetimes, 0, (RAM_SIZE + 1024) / 4 * sizeof(*s->lifetimes));
    memset(s->control_locations, 0, sizeof(s->control_locations));
    memset(s->pipeline, 0, sizeof(s->pipeline));
    memset(s->pipeline_location, 0, sizeof(s->pipeline_location));
    memset(s->instances, 0, sizeof(s->instances));
    s->projected_context = FRAME_TAG_UNKNOWN;
    ++s->reload_generation; ++s->pass;
    memset(s->expression_hash, 0, s->expression_capacity * sizeof(*s->expression_hash));
    s->expression_serial = 0;
    s->expression_live = s->free_expression = 0;
    s->collect_after = EXPR_COUNT * 2;
    memset(s->projections, 0, PROJECT_COUNT * sizeof(*s->projections));
    memset(s->polygon, 0, sizeof(s->polygon));
    s->initial_generation = ++s->generation;
    /* Restored registers have values but no surviving dataflow. They must not
     * become literal matrix words or silently inherit the old session's tags. */
    for (i = 0; i < 32; ++i) {
        s->cpu.r[i] = s->data[i] = s->control[i] = FRAME_TAG_UNKNOWN;
    }
    s->cpu.r[0] = 0;
    s->cpu.hi = s->cpu.lo = s->cpu.load = FRAME_TAG_UNKNOWN;
    s->frame = c->hw ? (unsigned)c->hw->vblanks : 0;
    s->gc_holds = 0;
    s->port = FRAME_TAG_UNKNOWN;
}

frame_register_tags_t *frame_tags_registers(cpu_t *c) { return &c->frame_tags->cpu; }

unsigned frame_tags_gc_hold(cpu_t *c) { return c->frame_tags ? c->frame_tags->gc_holds++ : 0; }
void frame_tags_gc_release(cpu_t *c, unsigned saved) { if (c->frame_tags) c->frame_tags->gc_holds = saved; }

static uint64_t hash_pair(uint64_t a, uint64_t b) {
    a ^= b + UINT64_C(0x9e3779b97f4a7c15) + (a << 6) + (a >> 2);
    a ^= a >> 30; a *= UINT64_C(0xbf58476d1ce4e5b9);
    a ^= a >> 27; a *= UINT64_C(0x94d049bb133111eb);
    return a ^ (a >> 31);
}

static int projected(frame_tag_t t) { return (t & TAG_KIND) == TAG_PROJECT; }

static expression_t *node(struct frame_tags *s, frame_tag_t t) {
    uint64_t id = t & ~TAG_KIND;
    return (t & TAG_KIND) == TAG_EXPR && id && id <= s->expression_serial &&
        s->expressions[id].split != UINT32_MAX ? &s->expressions[id] : NULL;
}

static int grow_expressions(struct frame_tags *s) {
    unsigned i, capacity = s->expression_capacity * 2;
    uint32_t *next;
    if (capacity > (1u << 24)) return 0;
    next = calloc(capacity, sizeof(*next));
    if (!next) return 0;
    for (i = 1; i <= s->expression_serial; ++i) {
        expression_t *e = &s->expressions[i];
        if (e->split == UINT32_MAX) continue;
        unsigned slot = (unsigned)hash_pair(hash_pair(e->a, e->b), e->split) & (capacity - 1);
        while (next[slot]) slot = (slot + 1) & (capacity - 1);
        next[slot] = i;
    }
    free(s->expression_hash); s->expression_hash = next; s->expression_capacity = capacity;
    return 1;
}

static frame_tag_t intern(cpu_t *c, frame_tag_t a, frame_tag_t b, unsigned split) {
    struct frame_tags *s = c->frame_tags;
    unsigned mask = s->expression_capacity - 1;
    unsigned slot = (unsigned)hash_pair(hash_pair(a, b), split) & mask;
    while (s->expression_hash[slot]) {
        unsigned id = s->expression_hash[slot];
        expression_t *e = &s->expressions[id];
        if (e->a == a && e->b == b && e->split == split) return TAG_EXPR | id;
        slot = (slot + 1) & mask;
    }
    if (s->expression_live >= s->expression_capacity * 7u / 10u) {
        if (grow_expressions(s)) return intern(c, a, b, split);
        ++s->exhausted; return FRAME_TAG_UNKNOWN;
    }
    if (!s->free_expression && s->expression_serial + 1 == s->node_capacity) {
        unsigned capacity = s->node_capacity * 2;
        expression_t *next = realloc(s->expressions, capacity * sizeof(*next));
        if (!next) { ++s->exhausted; return FRAME_TAG_UNKNOWN; }
        s->expressions = next; s->node_capacity = capacity;
    }
    {
        unsigned id = s->free_expression;
        if (id) s->free_expression = (unsigned)s->expressions[id].a;
        else id = (unsigned)++s->expression_serial;
        ++s->expression_live;
        expression_t *e = &s->expressions[id], *left = node(s, a);
        e->a = a; e->b = b; e->split = split;
        e->minimum = split && left && left->split ? left->minimum : a;
        s->expression_hash[slot] = id;
        return TAG_EXPR | id;
    }
}

/* Collection happens only at a retired VBlank boundary. IDs of live nodes do
 * not move, so RAM copies, retained projections, and temporal identities stay
 * stable. Dead IDs can be recycled only after every shadow root was traced.
 * An explicit work list also handles long guest arithmetic chains without
 * recursing on the host stack. Allocation failure leaves the old table intact. */
static void collect_expressions(cpu_t *c) {
    struct frame_tags *s = c->frame_tags;
    unsigned count = (unsigned)s->expression_serial + 1, i, top = 0, live = 0;
    unsigned *pending = malloc((size_t)count * sizeof(*pending));
    uint8_t *marked = calloc(count, 1);
    frame_register_tags_t *r = &s->cpu;
    if (!pending || !marked) { free(pending); free(marked); return; }
#define MARK(value) do { \
    frame_tag_t t_ = (value); unsigned id_ = (unsigned)(t_ & ~TAG_KIND); \
    if ((t_ & TAG_KIND) == TAG_EXPR && id_ && id_ < count && !marked[id_] && \
        s->expressions[id_].split != UINT32_MAX) { marked[id_] = 1; pending[top++] = id_; } \
} while (0)
    for (i = 0; i < 32; ++i) {
        MARK(r->r[i]); MARK(r->location[i]); MARK(s->data[i]); MARK(s->control[i]); MARK(s->control_locations[i]);
    }
    MARK(r->hi); MARK(r->lo); MARK(r->load); MARK(r->hi_location); MARK(r->lo_location); MARK(r->load_location);
    MARK(s->port); MARK(s->projected_context);
    for (i = 0; i < 256; ++i) { MARK(s->pipeline[i]); MARK(s->pipeline_location[i]); }
    for (i = 0; i < RAM_SIZE + 1024; ++i) MARK(s->memory[i]);
    for (i = 0; i < (RAM_SIZE + 1024) / 4; ++i) MARK(s->locations[i]);
    for (i = 0; i < PROJECT_COUNT; ++i) if (s->projections[i].serial) {
        MARK(s->projections[i].vertex); MARK(s->projections[i].context);
    }
    for (i = 0; i < 4096; ++i) MARK(s->instances[i].context);
    while (top) {
        expression_t *e = &s->expressions[pending[--top]];
        MARK(e->a); MARK(e->b);
    }
#undef MARK
    s->free_expression = 0;
    memset(s->expression_hash, 0, s->expression_capacity * sizeof(*s->expression_hash));
    for (i = 1; i < count; ++i) {
        expression_t *e = &s->expressions[i];
        if (marked[i]) {
            unsigned slot = (unsigned)hash_pair(hash_pair(e->a, e->b), e->split) & (s->expression_capacity - 1);
            while (s->expression_hash[slot]) slot = (slot + 1) & (s->expression_capacity - 1);
            s->expression_hash[slot] = i; ++live;
        } else {
            e->a = s->free_expression; e->b = e->minimum = 0; e->split = UINT32_MAX;
            s->free_expression = i;
        }
    }
    s->reclaimed += s->expression_live - live; s->expression_live = live;
    s->collect_after = live + (live / 4 > EXPR_COUNT ? live / 4 : EXPR_COUNT);
    ++s->collections;
    free(pending); free(marked);
}

static frame_tag_t expression(cpu_t *c, frame_tag_t a, frame_tag_t b) {
    return intern(c, a, b, 0);
}

/* Canonical Patricia sets make dependence union commutative and idempotent:
 * repeated scalar accumulation cannot manufacture a new identity merely by
 * nesting the same origins in a different order. Ordered transform tuples use
 * split=0 and remain atoms of a dependence set. */
static frame_tag_t unite(cpu_t *c, frame_tag_t a, frame_tag_t b) {
    struct frame_tags *s = c->frame_tags;
    expression_t *ap = node(s, a), *bp = node(s, b);
    unsigned as = ap ? ap->split : 0, bs = bp ? bp->split : 0, split = 0;
    frame_tag_t ak = as ? ap->minimum : a, bk = bs ? bp->minimum : b, diff = ak ^ bk;
    frame_tag_t al = as ? ap->a : 0, ar = as ? ap->b : 0;
    frame_tag_t bl = bs ? bp->a : 0, br = bs ? bp->b : 0;
    if (a == b) return a;
    while (diff) { ++split; diff >>= 1; }
    if (split > as && split > bs)
        return ak < bk ? intern(c, a, b, split) : intern(c, b, a, split);
    if (as == bs) {
        frame_tag_t left = unite(c, al, bl), right = unite(c, ar, br);
        if (left == FRAME_TAG_UNKNOWN || right == FRAME_TAG_UNKNOWN) return FRAME_TAG_UNKNOWN;
        return intern(c, left, right, as);
    }
    if (as > bs) {
        if ((bk >> (as - 1)) & 1) ar = unite(c, ar, b); else al = unite(c, al, b);
        if (al == FRAME_TAG_UNKNOWN || ar == FRAME_TAG_UNKNOWN) return FRAME_TAG_UNKNOWN;
        return intern(c, al, ar, as);
    }
    if ((ak >> (bs - 1)) & 1) br = unite(c, a, br); else bl = unite(c, a, bl);
    if (bl == FRAME_TAG_UNKNOWN || br == FRAME_TAG_UNKNOWN) return FRAME_TAG_UNKNOWN;
    return intern(c, bl, br, bs);
}

frame_tag_t frame_tags_merge(cpu_t *c, frame_tag_t a, frame_tag_t b) {
    if (a == b || !b) return a;
    if (!a) return b;
    if (a == FRAME_TAG_MIXED || b == FRAME_TAG_MIXED) return FRAME_TAG_MIXED;
    if (a == FRAME_TAG_UNKNOWN || b == FRAME_TAG_UNKNOWN) return FRAME_TAG_UNKNOWN;
    if (projected(a) && projected(b)) return FRAME_TAG_MIXED;
    if (projected(a)) return a;
    if (projected(b)) return b;
    return unite(c, a, b);
}

static frame_tag_t leaf(struct frame_tags *s, unsigned p) {
    return (s->generation << TAG_ADDRESS_BITS) | (p / 4 + 1);
}

frame_tag_t frame_tags_load(cpu_t *c, uint32_t address, unsigned width) {
    struct frame_tags *s = c->frame_tags;
    unsigned p = offset(address), i;
    frame_tag_t tag = 0;
    if (!s || p == UINT32_MAX || !width || width > RAM_SIZE + 1024 - p) return 0;
    for (i = 0; i < width; ++i) {
        if (!s->memory[p + i]) s->memory[p + i] = (s->initial_generation << TAG_ADDRESS_BITS) | ((p + i) / 4 + 1);
        tag = frame_tags_merge(c, tag, s->memory[p + i] == TAG_LITERAL ? 0 : s->memory[p + i]);
    }
    if (width < 4 && tag && !(tag & TAG_KIND))
        tag = expression(c, tag, (UINT64_C(1) << 61) | (width << 8) | (address & 3));
    return tag;
}

void frame_tags_invalidate(cpu_t *c, uint32_t address, unsigned width) {
    struct frame_tags *s = c->frame_tags;
    unsigned p = offset(address), i;
    if (!s || p == UINT32_MAX || !width || width > RAM_SIZE + 1024 - p) return;
    ++s->generation;
    for (i = p / 4; i <= (p + width - 1) / 4; ++i) s->locations[i] = 0;
    for (i = 0; i < width; ++i) s->memory[p + i] = leaf(s, p + i);
}

void frame_tags_store(cpu_t *c, uint32_t address, unsigned width, frame_tag_t tag) {
    struct frame_tags *s = c->frame_tags;
    unsigned p = offset(address), i;
    if (!s || c->halted || p == UINT32_MAX || width > RAM_SIZE + 1024 - p) return;
    if (p < RAM_SIZE && (c->cop0[12] & 0x10000u)) return;
    if (!tag) tag = TAG_LITERAL;
    for (i = 0; i < width; ++i) s->memory[p + i] = tag;
}

frame_tag_t frame_tags_location(cpu_t *c, uint32_t address) {
    struct frame_tags *s = c->frame_tags;
    unsigned p = offset(address);
    if (!s || p == UINT32_MAX) return 0;
    p /= 4;
    if (s->locations[p]) return s->locations[p];
    return ((s->lifetimes[p] ? s->lifetimes[p] : 1) << TAG_ADDRESS_BITS) | (p + 1);
}

void frame_tags_location_store(cpu_t *c, uint32_t address, unsigned width, frame_tag_t location) {
    struct frame_tags *s = c->frame_tags;
    unsigned p = offset(address);
    if (s && !c->halted && p != UINT32_MAX && width == 4 && !(address & 3) && !(p < RAM_SIZE && (c->cop0[12] & 0x10000u))) s->locations[p / 4] = location;
}

void frame_tags_reload(cpu_t *c, uint32_t address, unsigned width) {
    struct frame_tags *s = c->frame_tags;
    unsigned p = offset(address), i;
    if (!s || p == UINT32_MAX || !width || width > RAM_SIZE + 1024 - p) return;
    ++s->reload_generation;
    for (i = p / 4; i <= (p + width - 1) / 4; ++i) {
        s->lifetimes[i] = s->reload_generation; s->locations[i] = 0;
    }
}

void frame_tags_location_retire(cpu_t *c, unsigned dst, frame_tag_t value, unsigned load, frame_tag_t loaded) {
    frame_register_tags_t *r;
    if (!c->frame_tags) return;
    r = &c->frame_tags->cpu;
    if (c->load_reg && (c->halted || (c->load_reg != dst && c->load_reg != load))) r->location[c->load_reg] = r->load_location;
    if (!c->halted && dst) r->location[dst] = value;
    r->load_location = load && !c->halted ? loaded : 0;
    r->location[0] = 0;
}

frame_tag_t frame_tags_control_source(cpu_t *c, unsigned reg) {
    return c->frame_tags ? c->frame_tags->control_locations[reg] : 0;
}

void frame_tags_control_location(cpu_t *c, unsigned reg, frame_tag_t location) {
    if (c->frame_tags) c->frame_tags->control_locations[reg] = location;
}

void frame_tags_retire(cpu_t *c, unsigned dst, frame_tag_t value, unsigned load, frame_tag_t loaded) {
    frame_register_tags_t *r;
    if (!c->frame_tags) return;
    r = &c->frame_tags->cpu;
    if (c->load_reg && (c->halted || (c->load_reg != dst && c->load_reg != load))) r->r[c->load_reg] = r->load;
    if (!c->halted && dst) r->r[dst] = value;
    r->load = load && !c->halted ? loaded : 0;
    r->r[0] = 0;
}

void frame_tags_exception(cpu_t *c) {
    if (!c->frame_tags) return;
    if (c->load_reg) {
        c->frame_tags->cpu.r[c->load_reg] = c->frame_tags->cpu.load;
        c->frame_tags->cpu.location[c->load_reg] = c->frame_tags->cpu.load_location;
    }
    c->frame_tags->cpu.load = c->frame_tags->cpu.load_location = 0;
}

frame_tag_t frame_tags_gte_read(cpu_t *c, unsigned reg, int control) {
    struct frame_tags *s = c->frame_tags;
    if (!s) return 0;
    if (control) return s->control[reg];
    if (reg == 15) reg = 14;
    if (reg == 28 || reg == 29) return frame_tags_merge(c, s->data[9], frame_tags_merge(c, s->data[10], s->data[11]));
    return s->data[reg];
}

void frame_tags_gte_write(cpu_t *c, unsigned reg, int control, frame_tag_t tag) {
    struct frame_tags *s = c->frame_tags;
    if (!s) return;
    if (control) { s->control[reg] = tag; if (reg < 8) ++s->matrix_serial; return; }
    if (reg == 29 || reg == 31) return;
    if (reg == 15) { s->data[12] = s->data[13]; s->data[13] = s->data[14]; s->data[14] = tag; }
    if (reg == 28) s->data[9] = s->data[10] = s->data[11] = tag;
    if (reg == 30) s->data[31] = tag;
    s->data[reg] = tag;
}

static frame_tag_t vector_tag(cpu_t *c, unsigned v) {
    struct frame_tags *s = c->frame_tags;
    if (v == 3) return frame_tags_merge(c, s->data[9], frame_tags_merge(c, s->data[10], s->data[11]));
    return frame_tags_merge(c, s->data[v * 2], s->data[v * 2 + 1]);
}

static frame_tag_t context_tag(cpu_t *c, unsigned mx, unsigned cv) {
    struct frame_tags *s = c->frame_tags;
    unsigned i;
    frame_tag_t tag = 0;
    if (mx == 3) return FRAME_TAG_UNKNOWN;
    for (i = 0; i < 8; ++i) {
        unsigned reg = i < 5 ? mx * 8 + i : cv * 8 + i;
        frame_tag_t t;
        if (i >= 5 && cv == 3) continue;
        t = s->control_locations[reg];
        if (!t && s->control[reg]) return FRAME_TAG_UNKNOWN;
        /* Literal matrix words have no load origin. Retain their bits so a
         * literal transform change cannot alias an earlier context. */
        if (!t) t = expression(c, 0, (UINT64_C(1) << 61) | c->gte_ctrl[reg]);
        if (t == FRAME_TAG_UNKNOWN || t == FRAME_TAG_MIXED) return t;
        if (projected(t)) {
            uint64_t serial = t & ~TAG_KIND;
            projection_t *p = &s->projections[serial % PROJECT_COUNT];
            if (p->serial != serial) return FRAME_TAG_UNKNOWN;
            t = expression(c, p->vertex, p->context);
        }
        tag = expression(c, tag, t);
    }
    return tag;
}

void frame_tags_gte_command(cpu_t *c, uint32_t word) {
    struct frame_tags *s = c->frame_tags;
    unsigned cmd = word & 63, i;
    frame_tag_t tag;
    if (!s) return;
    s->control[31] = 0;
    if (cmd == 1 || cmd == 0x30) return; /* Actual project() publishes each FIFO slot. */
    if (cmd == 0x12) {
        tag = vector_tag(c, (word >> 15) & 3);
        tag = frame_tags_merge(c, tag, context_tag(c, (word >> 17) & 3, (word >> 13) & 3));
        for (i = 0; i < 3; ++i) s->data[9 + i] = s->data[25 + i] = tag;
        return;
    }
    if (cmd == 6) {
        s->data[24] = frame_tags_merge(c, s->data[12], frame_tags_merge(c, s->data[13], s->data[14]));
        return;
    }
    if (cmd == 0x2d || cmd == 0x2e) {
        tag = 0;
        for (i = cmd == 0x2d ? 17 : 16; i < 20; ++i) tag = frame_tags_merge(c, tag, s->data[i]);
        s->data[7] = s->data[24] = tag;
        return;
    }
    tag = vector_tag(c, 3);
    switch (cmd) {
    case 0x0c:
        for (i = 0; i < 3; ++i) {
            unsigned a = (i + 1) % 3, b = (i + 2) % 3;
            s->data[25 + i] = frame_tags_merge(c,
                frame_tags_merge(c, s->data[9 + b], s->control[a * 2]),
                frame_tags_merge(c, s->data[9 + a], s->control[b * 2]));
        }
        for (i = 0; i < 3; ++i) s->data[9 + i] = s->data[25 + i];
        return;
    case 0x28:
        for (i = 0; i < 3; ++i) s->data[25 + i] = s->data[9 + i];
        return;
    case 0x3d: case 0x3e:
        for (i = 0; i < 3; ++i) {
            frame_tag_t t = frame_tags_merge(c, s->data[9 + i], s->data[8]);
            if (cmd == 0x3e) t = frame_tags_merge(c, t, s->data[25 + i]);
            s->data[9 + i] = s->data[25 + i] = t;
        }
        tag = vector_tag(c, 3);
        break;
    case 0x13: case 0x16: case 0x1b: case 0x3f: case 0x1e: case 0x20:
        tag = vector_tag(c, cmd == 0x16 || cmd == 0x3f || cmd == 0x20 ? 2 : 0);
        tag = frame_tags_merge(c, tag, context_tag(c, 1, 3));
        tag = frame_tags_merge(c, tag, context_tag(c, 2, 1));
        if (cmd != 0x1e && cmd != 0x20) tag = frame_tags_merge(c, tag, s->data[6]);
        if (cmd == 0x13 || cmd == 0x16) tag = frame_tags_merge(c, tag, s->data[8]);
        break;
    case 0x14: case 0x1c:
        tag = frame_tags_merge(c, tag, context_tag(c, 2, 1));
        tag = frame_tags_merge(c, tag, s->data[6]);
        if (cmd == 0x14) tag = frame_tags_merge(c, tag, s->data[8]);
        break;
    case 0x10: case 0x2a:
        tag = s->data[cmd == 0x10 ? 6 : 22];
        tag = frame_tags_merge(c, tag, s->data[8]);
        break;
    case 0x11: tag = frame_tags_merge(c, tag, s->data[8]); break;
    case 0x29: tag = frame_tags_merge(c, tag, frame_tags_merge(c, s->data[6], s->data[8])); break;
    default: return;
    }
    if (cmd == 0x10 || cmd == 0x2a || cmd == 0x11 || cmd == 0x29 || cmd == 0x13 || cmd == 0x16 || cmd == 0x14)
        for (i = 21; i < 24; ++i) tag = frame_tags_merge(c, tag, s->control[i]);
    for (i = 0; i < 3; ++i) s->data[9 + i] = s->data[25 + i] = tag;
    if (cmd == 0x16 || cmd == 0x3f || cmd == 0x20 || cmd == 0x2a) s->data[20] = s->data[21] = s->data[22] = tag;
    else { s->data[20] = s->data[21]; s->data[21] = s->data[22]; s->data[22] = tag; }

}

void frame_tags_project(cpu_t *c, unsigned v, uint32_t xy, int64_t x, int64_t y, uint32_t depth) {
    struct frame_tags *s = c->frame_tags;
    frame_tag_t tag, vertex, context;
    projection_t *p;
    unsigned i;
    if (!s) {
        if (c->frame_tags_paused) ++c->frame_tags_paused->projection_serial;
        return;
    }
    ++s->projection_serial;
    vertex = vector_tag(c, v); context = context_tag(c, 0, 0);
    /* Input roles matter: (XY=A,Z=A) and (XY=A,Z=literal) are different
     * vertices even though their unordered dependence sets are identical. */
    if (vertex && vertex != FRAME_TAG_UNKNOWN && vertex != FRAME_TAG_MIXED && !projected(vertex))
        vertex = expression(c, s->data[v * 2], s->data[v * 2 + 1]);
    if (s->matrix_serial != s->projected_matrix || context != s->projected_context || s->pass != s->projected_pass) {
        unsigned slot = (unsigned)hash_pair(context, 0) & 4095, tries;
        s->occurrence = UINT32_MAX;
        for (tries = 0; tries < 4096; ++tries) {
            unsigned at = (slot + tries) & 4095;
            if (s->instances[at].pass != s->pass + 1 || s->instances[at].context == context) {
                if (s->instances[at].pass != s->pass + 1) {
                    s->instances[at].pass = s->pass + 1;
                    s->instances[at].context = context; s->instances[at].count = 0;
                }
                s->occurrence = s->instances[at].count++;
                break;
            }
        }
        s->projected_matrix = s->matrix_serial; s->projected_context = context; s->projected_pass = s->pass;
    }
    if (!vertex || vertex == FRAME_TAG_UNKNOWN || context == FRAME_TAG_UNKNOWN) tag = FRAME_TAG_UNKNOWN;
    else if (vertex == FRAME_TAG_MIXED || context == FRAME_TAG_MIXED || projected(vertex)) tag = FRAME_TAG_MIXED;
    else {
        uint64_t serial = s->projection_serial;
        p = &s->projections[serial % PROJECT_COUNT];
        p->serial = serial; p->vertex = vertex; p->context = context;
        p->pc = c->pc; p->xy = xy; p->x = x; p->y = y; p->depth = depth; p->frame = s->frame; p->occurrence = s->occurrence;
        p->input_xy = c->gte_data[v * 2]; p->input_z = c->gte_data[v * 2 + 1] & 0xffff;
        memcpy(p->matrix, c->gte_ctrl, sizeof(p->matrix));
        tag = TAG_PROJECT | serial;
    }
    s->data[12] = s->data[13]; s->data[13] = s->data[14]; s->data[14] = tag;
    s->data[16] = s->data[17]; s->data[17] = s->data[18]; s->data[18] = s->data[19]; s->data[19] = tag;
    for (i = 0; i < 3; ++i) s->data[9 + i] = s->data[25 + i] = tag;
    s->data[8] = s->data[24] = tag;
    if (s->trace && s->frame >= s->trace_first)
        fprintf(s->trace, "P,%u,%u,%u,0,%llu,%llu,%llu,%u,%lld,%lld,%u,%u,%u,%u\n", s->frame, s->pass, v,
            (unsigned long long)tag, (unsigned long long)vertex, (unsigned long long)context, c->pc,
            (long long)x, (long long)y, depth, c->gte_data[v * 2], c->gte_data[v * 2 + 1], s->occurrence);
}

void frame_tags_port(cpu_t *c, frame_tag_t tag) { if (c->frame_tags) c->frame_tags->port = tag; }
frame_tag_t frame_tags_port_read(cpu_t *c) { return c->frame_tags ? c->frame_tags->port : FRAME_TAG_UNKNOWN; }

void frame_tags_polygon(cpu_t *c, unsigned op, const uint32_t *words, const frame_tag_t *tags, unsigned n, int ox, int oy) {
    struct frame_tags *s = c ? c->frame_tags : NULL;
    unsigned i;
    if (!s) return;
    memset(s->polygon, 0, sizeof(s->polygon));
    for (i = 0; i < n; ++i) {
        frame_tag_t tag = tags[i];
        projection_t *p = NULL;
        unsigned kind = tag == FRAME_TAG_UNKNOWN ? 3 : tag == FRAME_TAG_MIXED ? 2 : projected(tag) ? 1 : 0;
        if (kind == 1) {
            uint64_t serial = tag & ~TAG_KIND;
            p = &s->projections[serial % PROJECT_COUNT];
            if (p->serial != serial) { kind = 3; p = NULL; }
        }
        ++s->corners[kind];
        if (p) {
            frame_corner_t *corner = &s->polygon[i];
            corner->vertex = p->vertex; corner->context = p->context;
            corner->generation = s->initial_generation; corner->collection = s->collections;
            corner->pc = p->pc; corner->occurrence = p->occurrence;
            corner->input_xy = p->input_xy; corner->input_z = p->input_z;
            memcpy(corner->matrix, p->matrix, sizeof(corner->matrix));
        }
        if (s->trace && s->frame >= s->trace_first)
            fprintf(s->trace, "C%u,%u,%u,%u,%u,%llu,%llu,%llu,%u,%d,%d,%u,%d,%d,%u\n", kind, s->frame, s->pass, i, op,
                (unsigned long long)tag, (unsigned long long)(p ? p->vertex : 0), (unsigned long long)(p ? p->context : 0),
                p ? p->pc : c->pc, (int16_t)words[i] + ox, (int16_t)(words[i] >> 16) + oy, p ? p->depth : 0, ox, oy, p ? p->occurrence : 0);
    }
}

int frame_tags_triangle(cpu_t *c, unsigned second, frame_corner_t out[3]) {
    struct frame_tags *s = c ? c->frame_tags : NULL;
    unsigned i;
    memset(out, 0, sizeof(*out) * 3);
    if (!s || second > 1) return 0;
    memcpy(out, s->polygon + second, sizeof(*out) * 3);
    for (i = 0; i < 3; ++i) if (!out[i].vertex || !out[i].context) return 0;
    return 1;
}

void frame_tags_swap(cpu_t *c, uint32_t display) {
    struct frame_tags *s = c ? c->frame_tags : NULL;
    if (s && s->display != display) { ++s->pass; s->display = display; }
}
void frame_tags_vblank(cpu_t *c) {
    struct frame_tags *s = c ? c->frame_tags : NULL;
    if (!s) return;
    ++s->frame;
    if (!(s->frame % 60) && !s->gc_holds && s->expression_live > s->collect_after) collect_expressions(c);
    if (s->trace && !(s->frame % 60)) fflush(s->trace);
}

void frame_tags_thread_save(cpu_t *c, uint32_t tcb) {
    unsigned i;
    if (!c->frame_tags) return;
    for (i = 0; i < 32; ++i) {
        frame_tags_store(c, tcb + 8 + i * 4, 4, c->frame_tags->cpu.r[i]);
        frame_tags_location_store(c, tcb + 8 + i * 4, 4, c->frame_tags->cpu.location[i]);
    }
    frame_tags_store(c, tcb + 0x8c, 4, c->frame_tags->cpu.hi);
    frame_tags_store(c, tcb + 0x90, 4, c->frame_tags->cpu.lo);
    frame_tags_location_store(c, tcb + 0x8c, 4, c->frame_tags->cpu.hi_location);
    frame_tags_location_store(c, tcb + 0x90, 4, c->frame_tags->cpu.lo_location);
}
void frame_tags_thread_restore(cpu_t *c, uint32_t tcb) {
    unsigned i;
    if (!c->frame_tags) return;
    for (i = 1; i < 32; ++i) {
        c->frame_tags->cpu.r[i] = frame_tags_load(c, tcb + 8 + i * 4, 4);
        c->frame_tags->cpu.location[i] = frame_tags_location(c, tcb + 8 + i * 4);
    }
    c->frame_tags->cpu.hi = frame_tags_load(c, tcb + 0x8c, 4);
    c->frame_tags->cpu.lo = frame_tags_load(c, tcb + 0x90, 4);
    c->frame_tags->cpu.hi_location = frame_tags_location(c, tcb + 0x8c);
    c->frame_tags->cpu.lo_location = frame_tags_location(c, tcb + 0x90);
}
void frame_tags_pipeline_save(cpu_t *c, unsigned index) {
    if (c->frame_tags && index < 256) {
        c->frame_tags->pipeline[index] = c->frame_tags->cpu.load;
        c->frame_tags->pipeline_location[index] = c->frame_tags->cpu.load_location;
    }
}
void frame_tags_pipeline_restore(cpu_t *c, unsigned index) {
    if (c->frame_tags) {
        c->frame_tags->cpu.load = index < 256 ? c->frame_tags->pipeline[index] : 0;
        c->frame_tags->cpu.load_location = index < 256 ? c->frame_tags->pipeline_location[index] : 0;
    }
}
#endif
