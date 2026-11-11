#include "frame_oracle.h"
#ifdef FF8_FRAME_ORACLE
#include "hw.h"
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* A root names where a 16-bit value was copied from. src is a physical RAM or
 * scratchpad byte address plus one, with gen counting the writes to that
 * halfword that were not copies (gen's top bit marks a value moved by an
 * untracked addend); or SCREEN plus an axis, with gen the serial of the
 * projection whose screen coordinate it is. 0 is unknown, ZERO a half known to
 * be zero by construction (after LHU, ANDI 0xffff or SLL 16). Registers carry
 * one root per half, so packing X and Y from two loads keeps both. */
typedef struct { uint32_t src, gen; } root_t;
typedef struct { root_t lo, hi; uint32_t value; } reg_t;
typedef struct { root_t r; uint16_t value; } half_t;
#define ZERO UINT32_MAX
#define SCREEN 0xc0000000u
#define OFFSET 0x80000000u
enum { HALVES = RAM_SIZE / 2 + 512 };

static int memory(root_t r) { return r.src && r.src != ZERO && (r.src & ~1u) != SCREEN; }
static int screen(root_t r, unsigned axis) { return r.src == (SCREEN | axis); }
static int zero(root_t r) { return r.src == ZERO; }

/* What the judge keeps of a projection. */
typedef struct {
    root_t coord[3], matrix[2]; /* the vertex; the translation's and rotation's first halves */
    int32_t view[3];
    int16_t input[3];
    uint8_t zero;               /* projected under a zero translation */
    uint32_t sxy, previous_sxy;
    int32_t match;
    uint8_t verdict;
} kept_t;

enum { CORRECT, PARTIAL, WRONG, EQUIVALENT, UNDECIDED, NONE };
enum { SCENES = 5 };
static const char *const scene_names[SCENES] = {"field", "world", "battle", "menu", "other"};

typedef struct {
    uint64_t frames, projections, matched, verdicts[5];
    uint64_t corners, linked, link_correct, link_equivalent, link_wrong, link_undecided, held_known, held_unknown;
    uint64_t instance_same, instance_other;
    /* Pairs of points under a zero translation in frames whose world offset
     * change the host gave (frame_oracle_world): of those the provenance
     * leaves undecided or partial, at one place and at two; of those it finds
     * wrong, at one place. */
    uint64_t place_correct, place_wrong, wrong_same_place;
} tally_t;

/* Open addressing from a 64-bit key to a 32-bit value. */
typedef struct { uint64_t *keys; uint32_t *values; size_t capacity; } map_t;

static struct {
    int active, lost;
    char lost_reason[160];
    cpu_t *cpu;
    FILE *out;
    /* Provenance. */
    reg_t reg[32], pending_reg, gte[6], ctrl[8], sxy[3];
    unsigned pending;
    half_t *mem;
    uint32_t *gen;
    /* Projections and polygons seen, not yet judged. */
    uint64_t serial, input_base, polygons, polygon_base;
    root_t (*inputs)[5];
    size_t input_capacity;
    int64_t *sources;              /* per polygon corner: projection serial, or -1 */
    size_t source_capacity;
    reg_t packet[12];
    int dma_pending;
    uint32_t dma_address;
    /* The previous judged frame. */
    kept_t *kept[2];
    size_t kept_count[2], kept_capacity[2];
    uint64_t kept_first[2], kept_number[2];
    int have_previous;
    map_t generations[2], by_root;
    int32_t *root_next;
    size_t root_next_capacity;
    tally_t total, scene[SCENES];
    unsigned logged[SCENES][2];
    int world_known;
    int32_t world_change[3];
} o;

/* Maps. */
static uint64_t mix(uint64_t h, uint64_t v) {
    h ^= v + UINT64_C(0x9e3779b97f4a7c15) + (h << 6) + (h >> 2);
    return h * UINT64_C(0xff51afd7ed558ccd);
}
static int map_reset(map_t *m, size_t entries) {
    size_t n = 64;
    while (n < entries * 2) n *= 2;
    if (n > m->capacity) {
        uint64_t *k = realloc(m->keys, n * sizeof(*k));
        uint32_t *v;
        if (!k) return 0;
        m->keys = k;
        if (!(v = realloc(m->values, n * sizeof(*v)))) return 0;
        m->values = v; m->capacity = n;
    }
    memset(m->keys, 0xff, m->capacity * sizeof(*m->keys));
    return 1;
}
static size_t map_slot(const map_t *m, uint64_t key) {
    size_t i = (size_t)(mix(0, key) & (m->capacity - 1));
    while (m->keys[i] != UINT64_MAX && m->keys[i] != key) i = (i + 1) & (m->capacity - 1);
    return i;
}
static int map_get(const map_t *m, uint64_t key, uint32_t *value) {
    size_t i;
    if (!m->capacity) return 0;
    i = map_slot(m, key);
    if (m->keys[i] == UINT64_MAX) return 0;
    *value = m->values[i];
    return 1;
}
static void map_put(map_t *m, uint64_t key, uint32_t value) {
    size_t i = map_slot(m, key);
    m->keys[i] = key; m->values[i] = value;
}
static int grow(void **items, size_t *capacity, size_t needed, size_t size) {
    size_t n = *capacity ? *capacity : 1024;
    void *p;
    if (needed <= *capacity) return 1;
    while (n < needed) n *= 2;
    if (!(p = realloc(*items, n * size))) return 0;
    *items = p; *capacity = n;
    return 1;
}

static void lose(const char *reason) {
    if (o.lost) return;
    o.lost = 1;
    snprintf(o.lost_reason, sizeof(o.lost_reason), "%s", reason);
}

/* Provenance. Halfwords: RAM mirrors fold onto RAM, the scratchpad follows it. */
static unsigned half_of(uint32_t address, uint32_t *physical) {
    uint32_t p = address & 0x1fffffffu;
    if (p < 0x800000u) { *physical = p & (RAM_SIZE - 1) & ~1u; return *physical / 2; }
    if (p - 0x1f800000u < 1024u) { *physical = 0x1f800000u + ((p - 0x1f800000u) & ~1u); return RAM_SIZE / 2 + (p & 1023) / 2; }
    return UINT32_MAX;
}

static uint16_t live_half(unsigned h) {
    uint16_t v;
    if (h < RAM_SIZE / 2) memcpy(&v, o.cpu->ram + h * 2, 2);
    else memcpy(&v, o.cpu->scratch + (h - RAM_SIZE / 2) * 2, 2);
    return v;
}

static root_t memory_root(uint32_t address) {
    root_t none = {0, 0};
    uint32_t physical;
    unsigned h = half_of(address, &physical);
    uint16_t live;
    if (h == UINT32_MAX) return none;
    live = live_half(h);
    if (o.mem[h].value != live || !o.mem[h].r.src) {
        /* First sight, or written by something the oracle does not watch. */
        if (o.mem[h].r.src) ++o.gen[h];
        o.mem[h].r.src = physical + 1; o.mem[h].r.gen = o.gen[h]; o.mem[h].value = live;
    }
    return o.mem[h].r;
}

static void memory_store(uint32_t address, root_t r) {
    uint32_t physical;
    unsigned h = half_of(address, &physical);
    if (h == UINT32_MAX) return;
    if (r.src && r.src != ZERO) o.mem[h].r = r;
    else { ++o.gen[h]; o.mem[h].r.src = physical + 1; o.mem[h].r.gen = o.gen[h]; }
    o.mem[h].value = live_half(h);
}

static void memory_self(uint32_t address) {
    root_t none = {0, 0};
    memory_store(address, none);
}

static reg_t none_reg(void) { reg_t r; memset(&r, 0, sizeof(r)); return r; }

static reg_t reg_get(unsigned reg, uint32_t live) {
    reg_t r = none_reg();
    if (!reg) { r.lo.src = r.hi.src = ZERO; return r; }
    if (o.reg[reg].value != live) return r;
    return o.reg[reg];
}

static int vz(unsigned reg) { return reg == 1 || reg == 3 || reg == 5; }

static reg_t checked(reg_t r, uint32_t live, int low_only) {
    if ((r.value ^ live) & 0xffffu) r.lo.src = 0;
    if (low_only || ((r.value ^ live) >> 16)) r.hi.src = 0;
    return r;
}
static reg_t gte_get(unsigned reg) { return checked(o.gte[reg], o.cpu->gte_data[reg], vz(reg)); }
static reg_t ctrl_get(unsigned reg) { return checked(o.ctrl[reg], o.cpu->gte_ctrl[reg], reg == 4); }
static reg_t sxy_get(unsigned i) { return checked(o.sxy[i], o.cpu->gte_data[12 + i], 0); }

/* The SXY FIFO, as gte_write and the projections move it. */
static void sxy_push(reg_t r) {
    o.sxy[0] = o.sxy[1]; o.sxy[1] = o.sxy[2]; o.sxy[2] = r;
}
static void sxy_write(unsigned reg, reg_t r) {
    r.value = o.cpu->gte_data[reg == 15 ? 14 : reg];
    if (reg == 15) sxy_push(r);
    else o.sxy[reg - 12] = r;
}

static void project(unsigned v) {
    reg_t xy = gte_get(v * 2), z = gte_get(v * 2 + 1), out;
    root_t *in;
    if (!grow((void **)&o.inputs, &o.input_capacity, (size_t)(o.serial - o.input_base + 1), sizeof(*o.inputs))) {
        lose("out of memory"); return;
    }
    in = o.inputs[o.serial - o.input_base];
    in[0] = xy.lo; in[1] = xy.hi; in[2] = z.lo;
    in[3] = ctrl_get(5).lo; in[4] = ctrl_get(0).lo;
    out.lo.src = SCREEN; out.hi.src = SCREEN | 1;
    out.lo.gen = out.hi.gen = (uint32_t)o.serial;
    out.value = 0;
    sxy_push(out);
    ++o.serial;
}

static root_t offset(root_t a, root_t b) {
    root_t none = {0, 0};
    int ka = memory(a), kb = memory(b);
    /* A sum with one untracked operand keeps the tracked one's source; a
     * screen coordinate moved by anything is no longer that coordinate. */
    if (ka == kb) return none;
    a = ka ? a : b;
    a.gen |= OFFSET;
    return a;
}

void frame_oracle_step(cpu_t *c, uint32_t word, uint32_t a, uint32_t b, uint32_t address,
                       unsigned dst, uint32_t result, unsigned load, uint32_t loaded) {
    unsigned op = word >> 26, rs = (word >> 21) & 31, rt = (word >> 16) & 31;
    unsigned rd = (word >> 11) & 31, sh = (word >> 6) & 31, fn = word & 63;
    uint32_t imm = word & 65535;
    reg_t value = none_reg(), from_load = none_reg(), ra, rb;
    if (!o.active || c->halted) return;
    if (c != o.cpu) { lose("the guest CPU changed without a restore"); return; }
    ra = reg_get(rs, a); rb = reg_get(rt, b);
    /* Results that keep halfword sources; anything else is computed. */
    if (op == 0) {
        if (fn == 33 || fn == 37 || fn == 38) {
            /* ADDU, OR, XOR: a half passes through when the other operand's half is zero.
             * ADDU can carry into the high half unless one low half is zero. */
            value.lo = zero(ra.lo) ? rb.lo : zero(rb.lo) ? ra.lo : value.lo;
            if (fn != 33 || zero(ra.lo) || zero(rb.lo))
                value.hi = zero(ra.hi) ? rb.hi : zero(rb.hi) ? ra.hi : value.hi;
            if (fn == 33 && !value.lo.src) value.lo = offset(ra.lo, rb.lo);
        } else if (fn == 35 && !rt) value = ra;
        else if (fn == 35 || fn == 34 || fn == 32) value.lo = offset(ra.lo, rb.lo);
        else if (fn == 0 && sh == 0) value = rb;
        else if (fn == 0 && sh == 16) { value.hi = rb.lo; value.lo.src = ZERO; }
        else if (fn == 2 && sh == 16) { value.lo = rb.hi; value.hi.src = ZERO; }
        else if (fn == 3 && sh == 16) value.lo = rb.hi;
    } else if ((op == 9 || op == 13) && !imm) value = ra;
    else if (op == 9 || op == 8) { root_t none0 = {0, 0}; value.lo = offset(ra.lo, none0); }
    else if (op == 12 && imm == 0xffff) { value.lo = ra.lo; value.hi.src = ZERO; }
    else if (op == 13) value.hi = ra.hi;
    else if (op == 15) value.lo.src = ZERO;
    /* Loads. */
    if (op == 35) { from_load.lo = memory_root(address); from_load.hi = memory_root(address + 2); }
    else if (op == 33 || op == 37) { from_load.lo = memory_root(address); if (op == 37) from_load.hi.src = ZERO; }
    else if (op == 34 || op == 38) {
        /* LWL/LWR: halfword-aligned pieces keep their sources; the rest of the
         * register is the merge value, which may be a pending load. */
        unsigned n = address & 3;
        from_load = o.pending == rt ? o.pending_reg : rb;
        if (op == 38 && n == 0) { from_load.lo = memory_root(address); from_load.hi = memory_root(address + 2); }
        else if (op == 38 && n == 2) from_load.lo = memory_root(address);
        else if (op == 38) { from_load.lo.src = 0; if (n == 1) from_load.hi.src = 0; }
        else if (op == 34 && n == 3) { from_load.lo = memory_root(address - 3); from_load.hi = memory_root(address - 1); }
        else if (op == 34 && n == 1) from_load.hi = memory_root(address - 1);
        else { from_load.hi.src = 0; if (n == 2) from_load.lo.src = 0; }
    }
    else if (op == 18 && rs == 0 && rd < 6) from_load = gte_get(rd);
    else if (op == 18 && rs == 0 && rd >= 12 && rd <= 15) from_load = sxy_get(rd == 15 ? 2 : rd - 12);
    else if (op == 18 && rs == 2 && rd < 8) from_load = ctrl_get(rd);
    from_load.value = loaded;
    /* Stores and coprocessor writes. */
    if (op == 43) { memory_store(address, rb.lo); memory_store(address + 2, rb.hi); }
    else if (op == 41) memory_store(address, rb.lo);
    else if (op == 46 && !(address & 3)) { memory_store(address, rb.lo); memory_store(address + 2, rb.hi); }
    else if (op == 46 && (address & 3) == 2) memory_store(address, rb.lo);
    else if (op == 42 && (address & 3) == 3) { memory_store(address - 3, rb.lo); memory_store(address - 1, rb.hi); }
    else if (op == 42 && (address & 3) == 1) memory_store(address - 1, rb.hi);
    else if (op == 40 || op == 42 || op == 46) { memory_self(address); memory_self(address + 2); }
    else if (op == 58) {
        reg_t g = rt < 6 ? gte_get(rt) : rt >= 12 && rt <= 15 ? sxy_get(rt == 15 ? 2 : rt - 12) : none_reg();
        memory_store(address, g.lo); memory_store(address + 2, g.hi);
    } else if (op == 50 && rt < 6) {
        o.gte[rt].lo = memory_root(address); o.gte[rt].hi = memory_root(address + 2);
        o.gte[rt].value = c->gte_data[rt];
    } else if (op == 50 && rt >= 12 && rt <= 15) {
        reg_t m = none_reg();
        m.lo = memory_root(address); m.hi = memory_root(address + 2);
        sxy_write(rt, m);
    } else if (op == 18 && rs == 4 && rd < 6) { o.gte[rd] = rb; o.gte[rd].value = c->gte_data[rd]; }
    else if (op == 18 && rs == 4 && rd >= 12 && rd <= 15) sxy_write(rd, rb);
    else if (op == 18 && rs == 6 && rd < 8) { o.ctrl[rd] = rb; o.ctrl[rd].value = c->gte_ctrl[rd]; }
    else if (op == 18 && rs >= 16) {
        /* The entries pushed out of the way keep the values they were
         * recorded with, so a stale one stays stale at its new place. */
        if ((word & 63) == 0x01) { project(0); o.sxy[2].value = c->gte_data[14]; }
        else if ((word & 63) == 0x30) {
            unsigned i;
            project(0); project(1); project(2);
            for (i = 0; i < 3; ++i) o.sxy[i].value = c->gte_data[12 + i];
        }
    }
    /* Mirror c_retire's load delay; record the value each register will hold. */
    if (o.pending && o.pending != dst && o.pending != load) o.reg[o.pending] = o.pending_reg;
    if (dst) { o.reg[dst] = value; o.reg[dst].value = result; }
    o.pending = load;
    o.pending_reg = from_load;
    o.reg[0] = none_reg();
}

/* GPU packets. */
void frame_oracle_dma_word(uint32_t address) {
    o.dma_pending = address != UINT32_MAX;
    o.dma_address = address;
}

void frame_oracle_packet_word(unsigned slot, const uint8_t *source) {
    reg_t r = none_reg();
    if (!o.active || slot >= 12) return;
    if (source && source >= o.cpu->ram && source < o.cpu->ram + RAM_SIZE) {
        uint32_t address = (uint32_t)(source - o.cpu->ram);
        r.lo = memory_root(address); r.hi = memory_root(address + 2);
    } else if (!source && o.dma_pending) {
        r.lo = memory_root(o.dma_address); r.hi = memory_root(o.dma_address + 2);
    }
    o.dma_pending = 0;
    o.packet[slot] = r;
}

void frame_oracle_polygon(unsigned opcode) {
    unsigned n = (opcode & 8) ? 4 : 3, i, at = 1;
    int64_t *s;
    if (!o.active) return;
    if (!grow((void **)&o.sources, &o.source_capacity, (size_t)(o.polygons - o.polygon_base + 1) * 4, sizeof(*o.sources))) {
        lose("out of memory"); return;
    }
    s = &o.sources[(o.polygons - o.polygon_base) * 4];
    for (i = 0; i < 4; ++i) s[i] = -1;
    for (i = 0; i < n; ++i) {
        const reg_t *w;
        if (i && (opcode & 16)) ++at;
        w = &o.packet[at++];
        if (opcode & 4) ++at;
        /* Both halves from one projection's two coordinates. */
        if (screen(w->lo, 0) && screen(w->hi, 1) && w->lo.gen == w->hi.gen) s[i] = w->lo.gen;
    }
    ++o.polygons;
}

/* Judging. */
static unsigned scene_of(uint32_t tag) {
    switch (tag & 0xffffu) {
    case 1: return 0;
    case 2: return 1;
    case 3: return 2;
    case 6: return 3;
    default: return 4;
    }
}

static int generations(map_t *m, const kept_t *k, size_t n) {
    size_t i;
    unsigned a;
    if (!map_reset(m, n * 3)) return 0;
    for (i = 0; i < n; ++i)
        for (a = 0; a < 3; ++a) if (memory(k[i].coord[a])) map_put(m, k[i].coord[a].src, k[i].coord[a].gen);
    return 1;
}

static int stable(root_t r, const map_t *other) {
    uint32_t gen;
    return map_get(other, r.src, &gen) && gen == r.gen;
}

static int full(const kept_t *k) { return memory(k->coord[0]) && memory(k->coord[1]) && memory(k->coord[2]); }
static uint64_t root_key(const kept_t *k) {
    uint64_t h = 0;
    unsigned a;
    for (a = 0; a < 3; ++a) h = mix(mix(h, k->coord[a].src), k->coord[a].gen);
    return h;
}
static int same_roots(const kept_t *a, const kept_t *b) {
    unsigned i;
    for (i = 0; i < 3; ++i) if (a->coord[i].src != b->coord[i].src || a->coord[i].gen != b->coord[i].gen) return 0;
    return 1;
}

static int judge_pair(const kept_t *a, const kept_t *b, const map_t *prev_gen, const map_t *cur_gen) {
    int eq = 0, diff = 0;
    unsigned i;
    for (i = 0; i < 3; ++i) {
        root_t x = a->coord[i], y = b->coord[i];
        if (!memory(x) || !memory(y)) continue;
        if (x.src == y.src && x.gen == y.gen) ++eq;
        else if (x.src != y.src && stable(x, cur_gen) && stable(y, prev_gen)) ++diff;
    }
    return diff ? WRONG : eq == 3 ? CORRECT : eq ? PARTIAL : UNDECIDED;
}

/* A projection by serial, in the frame being judged or the one before. */
static const kept_t *by_serial(uint64_t serial) {
    unsigned f;
    for (f = 0; f < 2; ++f)
        if (o.kept[f] && serial >= o.kept_first[f] && serial - o.kept_first[f] < o.kept_count[f])
            return &o.kept[f][serial - o.kept_first[f]];
    return NULL;
}

static int within_pixel(uint32_t a, uint32_t b) {
    int dx = (int16_t)(a & 0xffff) - (int16_t)(b & 0xffff), dy = (int16_t)(a >> 16) - (int16_t)(b >> 16);
    return dx >= -1 && dx <= 1 && dy >= -1 && dy <= 1;
}

static void add(tally_t *t, const tally_t *f) {
    unsigned i;
    t->frames += f->frames; t->projections += f->projections; t->matched += f->matched;
    for (i = 0; i < 5; ++i) t->verdicts[i] += f->verdicts[i];
    t->corners += f->corners; t->linked += f->linked; t->link_correct += f->link_correct;
    t->link_equivalent += f->link_equivalent; t->link_wrong += f->link_wrong; t->link_undecided += f->link_undecided;
    t->held_known += f->held_known; t->held_unknown += f->held_unknown;
    t->instance_same += f->instance_same; t->instance_other += f->instance_other;
    t->place_correct += f->place_correct; t->place_wrong += f->place_wrong; t->wrong_same_place += f->wrong_same_place;
}

void frame_oracle_judge(const frame_identity_frame_t *f) {
    kept_t *cur;
    const kept_t *prev = NULL;
    size_t i;
    unsigned c;
    tally_t t;
    if (!o.active || o.lost) return;
    if (f->first_projection != o.input_base || o.serial - o.input_base != f->projection_count ||
        f->first_polygon != o.polygon_base || o.polygons - o.polygon_base != f->polygon_count) {
        if (!(f->flags & FRAME_IDENTITY_OVERFLOW)) {
            char reason[160];
            snprintf(reason, sizeof(reason), "frame %" PRIu64 ": the service saw %u projections from %" PRIu64
                     ", the oracle %" PRIu64 " from %" PRIu64, f->number, f->projection_count, f->first_projection,
                     o.serial - o.input_base, o.input_base);
            lose(reason);
            return;
        }
    }
    /* Keep this frame as slot 0, the previous as slot 1. */
    {
        kept_t *swap = o.kept[1]; size_t cap = o.kept_capacity[1];
        o.kept[1] = o.kept[0]; o.kept_capacity[1] = o.kept_capacity[0];
        o.kept_count[1] = o.kept_count[0]; o.kept_first[1] = o.kept_first[0]; o.kept_number[1] = o.kept_number[0];
        o.kept[0] = swap; o.kept_capacity[0] = cap;
    }
    if (!grow((void **)&o.kept[0], &o.kept_capacity[0], f->projection_count, sizeof(kept_t))) { lose("out of memory"); return; }
    o.kept_count[0] = f->projection_count; o.kept_first[0] = f->first_projection; o.kept_number[0] = f->number;
    cur = o.kept[0];
    if (o.have_previous && o.kept_number[1] + 1 == f->number) prev = o.kept[1];
    for (i = 0; i < f->projection_count; ++i) {
        const frame_identity_projection_t *p = &f->projections[i];
        memcpy(cur[i].coord, o.inputs[i], sizeof(cur[i].coord));
        memcpy(cur[i].matrix, o.inputs[i] + 3, sizeof(cur[i].matrix));
        memcpy(cur[i].view, p->view, sizeof(cur[i].view));
        memcpy(cur[i].input, p->input, sizeof(cur[i].input));
        cur[i].zero = 0;
        if (p->run < f->run_count) {
            const int32_t *t = f->setups[f->runs[p->run].setup].translation;
            cur[i].zero = !t[0] && !t[1] && !t[2];
        }
        cur[i].sxy = p->sxy; cur[i].match = p->match; cur[i].verdict = NONE;
        cur[i].previous_sxy = prev && p->match >= 0 ? prev[p->match].sxy : 0;
    }
    memset(&t, 0, sizeof(t));
    t.frames = 1; t.projections = f->projection_count;
    if (prev) {
        size_t n = o.kept_count[1];
        if (!generations(&o.generations[0], cur, f->projection_count) || !generations(&o.generations[1], prev, n) ||
            !map_reset(&o.by_root, n) ||
            !grow((void **)&o.root_next, &o.root_next_capacity, n, sizeof(*o.root_next))) { lose("out of memory"); return; }
        for (i = n; i-- > 0;) {
            uint32_t head;
            if (!full(&prev[i])) continue;
            o.root_next[i] = map_get(&o.by_root, root_key(&prev[i]), &head) ? (int32_t)head : -1;
            map_put(&o.by_root, root_key(&prev[i]), (uint32_t)i);
        }
        for (i = 0; i < f->projection_count; ++i) {
            const kept_t *a;
            int v;
            if (cur[i].match < 0) continue;
            ++t.matched;
            a = &prev[cur[i].match];
            v = judge_pair(a, &cur[i], &o.generations[1], &o.generations[0]);
            if (v == WRONG && full(&cur[i])) {
                /* The oracle's own partner stood exactly where the mechanism's
                 * did: one point held in two records, harmless for drawing. */
                uint32_t head;
                int32_t j = map_get(&o.by_root, root_key(&cur[i]), &head) ? (int32_t)head : -1;
                for (; j >= 0; j = o.root_next[j])
                    if (same_roots(&prev[j], &cur[i]) && !memcmp(prev[j].view, a->view, sizeof(a->view)) &&
                        prev[j].sxy == a->sxy) { v = EQUIVALENT; break; }
            }
            cur[i].verdict = (uint8_t)v;
            /* Where the game moved both points by its world offset, the
             * offset's change says whether they are one place in the world:
             * that decides what provenance cannot (a point whose input was
             * moved by an addend it does not follow), and tells a wrong
             * record at the right place (an edge both blocks keep) from a
             * wrong place. */
            if (o.world_known && cur[i].zero && a->zero) {
                unsigned k;
                int same = 1;
                for (k = 0; k < 3; ++k) if (cur[i].input[k] - a->input[k] != o.world_change[k]) same = 0;
                if (v == UNDECIDED || v == PARTIAL) ++*(same ? &t.place_correct : &t.place_wrong);
                else if (v == WRONG && same) ++t.wrong_same_place;
                if ((v == UNDECIDED || v == PARTIAL) && !same && o.out && o.logged[scene_of(f->tag)][0] < 100) {
                    ++o.logged[scene_of(f->tag)][0];
                    fprintf(o.out, "W place frame=%" PRIu64 " vblank=%" PRIu64 " tag=%u #%zu<-#%d in=%d,%d,%d<-%d,%d,%d"
                            " change=%d,%d,%d\n", f->number, f->vblank, f->tag & 0xffffu, i, cur[i].match,
                            cur[i].input[0], cur[i].input[1], cur[i].input[2], a->input[0], a->input[1], a->input[2],
                            o.world_change[0], o.world_change[1], o.world_change[2]);
                }
            }
            /* Identical instances may share one vertex table, where the
             * vertex sources cannot tell them apart; where each instance's
             * matrix is copied from its own place, that place can. Only a
             * repeated key can be swapped with another instance. */
            if (v == CORRECT && f->runs[f->projections[i].run].repeats) {
                unsigned m = memory(cur[i].matrix[0]) && memory(a->matrix[0]) ? 0 : 1;
                if (memory(cur[i].matrix[m]) && memory(a->matrix[m])) {
                    if (cur[i].matrix[m].src == a->matrix[m].src) ++t.instance_same;
                    else {
                        ++t.instance_other;
                        if (o.out && o.logged[scene_of(f->tag)][0] < 100) {
                            ++o.logged[scene_of(f->tag)][0];
                            fprintf(o.out, "W instance frame=%" PRIu64 " vblank=%" PRIu64 " tag=%u #%zu<-#%d run=%u repeats=%u"
                                    " matrix%u=%08x<-%08x\n", f->number, f->vblank, f->tag & 0xffffu, i, cur[i].match,
                                    f->projections[i].run, f->runs[f->projections[i].run].repeats, m,
                                    cur[i].matrix[m].src, a->matrix[m].src);
                        }
                    }
                }
            }
            ++t.verdicts[v];
            if (v == WRONG && o.out && o.logged[scene_of(f->tag)][0] < 100) {
                ++o.logged[scene_of(f->tag)][0];
                fprintf(o.out, "W pair frame=%" PRIu64 " vblank=%" PRIu64 " tag=%u #%zu<-#%d run=%u repeats=%u"
                        " x=%08x/%u<-%08x/%u z=%08x/%u<-%08x/%u\n", f->number, f->vblank, f->tag & 0xffffu, i, cur[i].match,
                        f->projections[i].run, f->runs[f->projections[i].run].repeats,
                        cur[i].coord[0].src, cur[i].coord[0].gen, a->coord[0].src, a->coord[0].gen,
                        cur[i].coord[2].src, cur[i].coord[2].gen, a->coord[2].src, a->coord[2].gen);
            }
        }
    }
    for (i = 0; i < f->polygon_count; ++i) {
        const frame_identity_polygon_t *q = &f->polygons[i];
        const int64_t *s = o.sources ? &o.sources[i * 4] : NULL;
        for (c = 0; c < q->corners; ++c) {
            int64_t truth = s ? s[c] : -1;
            int linked = q->state[c] == FRAME_IDENTITY_LINKED || q->state[c] == FRAME_IDENTITY_EXCLUDED_CORNER;
            ++t.corners;
            if (!linked) { if (truth >= 0) ++t.held_known; else ++t.held_unknown; continue; }
            ++t.linked;
            if (truth < 0) { ++t.link_undecided; continue; }
            {
                uint64_t chosen = (q->age[c] ? o.kept_first[1] : f->first_projection) + (uint64_t)q->projection[c];
                const kept_t *mine = by_serial(chosen), *real = by_serial((uint64_t)truth);
                if (chosen == (uint64_t)truth) ++t.link_correct;
                /* Another record of the same point: both are paired and stood
                 * within a pixel of each other in the frame before, so the
                 * corner is drawn where the true link would draw it. Whether
                 * that pair is itself right is the pair verdict's business. */
                else if (mine && real && mine->sxy == real->sxy && mine->match >= 0 && real->match >= 0 &&
                         within_pixel(mine->previous_sxy, real->previous_sxy))
                    ++t.link_equivalent;
                else {
                    ++t.link_wrong;
                    if (o.out && o.logged[scene_of(f->tag)][1] < 100) {
                        ++o.logged[scene_of(f->tag)][1];
                        fprintf(o.out, "W link frame=%" PRIu64 " vblank=%" PRIu64 " tag=%u polygon=%zu corner=%u age=%u"
                                " chosen=%" PRIu64 " truth=%" PRId64 " sxy=%08x/%08x match=%d/%d verdict=%d/%d before=%08x/%08x\n",
                                f->number, f->vblank, f->tag & 0xffffu, i, c, q->age[c], chosen, truth,
                                mine ? mine->sxy : 0, real ? real->sxy : 0, mine ? mine->match : -9, real ? real->match : -9,
                                mine ? mine->verdict : -9, real ? real->verdict : -9,
                                mine ? mine->previous_sxy : 0, real ? real->previous_sxy : 0);
                    }
                }
            }
        }
    }
    add(&o.total, &t);
    add(&o.scene[scene_of(f->tag)], &t);
    if (o.out)
        fprintf(o.out, "F %" PRIu64 " %" PRIu64 " %u %u %" PRIu64 " %" PRIu64 " %" PRIu64 " %" PRIu64 " %" PRIu64 " %" PRIu64
                " %" PRIu64 " %" PRIu64 " %" PRIu64 " %" PRIu64 " %" PRIu64 " %" PRIu64 " %" PRIu64 " %" PRIu64 "\n",
                f->number, f->vblank, f->tag & 0xffffu, f->flags, t.projections, t.matched, t.verdicts[CORRECT],
                t.verdicts[PARTIAL], t.verdicts[WRONG], t.verdicts[EQUIVALENT], t.verdicts[UNDECIDED], t.corners,
                t.linked, t.link_correct, t.link_equivalent, t.link_wrong, t.link_undecided, t.held_known);
    /* Drop what this frame consumed. */
    o.have_previous = !(f->flags & FRAME_IDENTITY_OVERFLOW);
    o.input_base = o.serial; o.polygon_base = o.polygons;
}

void frame_oracle_world(int known, const int32_t change[3]) {
    o.world_known = known;
    memcpy(o.world_change, change, sizeof(o.world_change));
}

static const frame_oracle_gpu_t gpu_hooks = {frame_oracle_packet_word, frame_oracle_polygon};

int frame_oracle_start(cpu_t *c, const char *report) {
    if (!c->interpreter_only) {
        fprintf(stderr, "--frame-oracle needs --interpreter: it follows values through interpreted instructions only\n");
        return 0;
    }
    memset(&o, 0, sizeof(o));
    o.mem = calloc(HALVES, sizeof(*o.mem));
    o.gen = calloc(HALVES, sizeof(*o.gen));
    if (!o.mem || !o.gen || !(o.out = fopen(report, "w"))) {
        fprintf(stderr, "Cannot start the frame oracle: %s\n", report);
        free(o.mem); free(o.gen); memset(&o, 0, sizeof(o));
        return 0;
    }
    fputs("# F number vblank tag flags projections matched correct partial wrong equivalent undecided"
          " corners linked link-correct link-equivalent link-wrong link-undecided held-known\n", o.out);
    o.cpu = c;
    o.active = 1;
    frame_oracle_gpu = &gpu_hooks;
    return 1;
}

void frame_oracle_restored(cpu_t *c) {
    if (!o.active) return;
    if (!c->interpreter_only) { lose("a restored state runs native code"); return; }
    /* Sources are checked against live values, so only what refers to the old
     * CPU must go. The service has dropped its history too. */
    o.cpu = c;
    memset(o.reg, 0, sizeof(o.reg)); memset(o.gte, 0, sizeof(o.gte)); memset(o.ctrl, 0, sizeof(o.ctrl));
    memset(o.sxy, 0, sizeof(o.sxy)); o.pending = 0; o.have_previous = 0;
}

static void totals(const char *label, const tally_t *t) {
    fprintf(o.out, "T %s frames=%" PRIu64 " projections=%" PRIu64 " matched=%" PRIu64 " correct=%" PRIu64 " partial=%" PRIu64
            " wrong=%" PRIu64 " equivalent=%" PRIu64 " undecided=%" PRIu64 " corners=%" PRIu64 " linked=%" PRIu64
            " link-correct=%" PRIu64 " link-equivalent=%" PRIu64 " link-wrong=%" PRIu64 " link-undecided=%" PRIu64
            " held-known=%" PRIu64 " held-unknown=%" PRIu64 " instance-same=%" PRIu64 " instance-other=%" PRIu64
            " place-correct=%" PRIu64 " place-wrong=%" PRIu64 " wrong-same-place=%" PRIu64 "\n",
            label, t->frames, t->projections, t->matched,
            t->verdicts[CORRECT], t->verdicts[PARTIAL], t->verdicts[WRONG], t->verdicts[EQUIVALENT], t->verdicts[UNDECIDED],
            t->corners, t->linked, t->link_correct, t->link_equivalent, t->link_wrong, t->link_undecided,
            t->held_known, t->held_unknown, t->instance_same, t->instance_other, t->place_correct, t->place_wrong, t->wrong_same_place);
}

int frame_oracle_stop(void) {
    int ok = 1;
    unsigned i;
    if (!o.active) return 1;
    frame_oracle_gpu = NULL;
    if (o.out) {
        totals("all", &o.total);
        for (i = 0; i < SCENES; ++i) if (o.scene[i].frames) totals(scene_names[i], &o.scene[i]);
        if (o.lost) fprintf(o.out, "LOST %s\n", o.lost_reason);
        if (fclose(o.out)) ok = 0;
    }
    if (o.lost) { fprintf(stderr, "Frame oracle lost track: %s\n", o.lost_reason); ok = 0; }
    free(o.mem); free(o.gen); free(o.inputs); free(o.sources); free(o.kept[0]); free(o.kept[1]);
    free(o.generations[0].keys); free(o.generations[0].values); free(o.generations[1].keys); free(o.generations[1].values);
    free(o.by_root.keys); free(o.by_root.values); free(o.root_next);
    memset(&o, 0, sizeof(o));
    return ok;
}
#endif
