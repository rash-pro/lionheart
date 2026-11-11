#include "frame_layer.h"
#include <stdlib.h>
#include <string.h>

/* What a sprite does with its layer. */
enum { KIND_LAYER, KIND_UNPAIRED, KIND_HELD, KIND_DISAGREE };

typedef struct {
    uint64_t key;                /* what it draws; 0 for a rectangle without a texture */
    int32_t x, y;                /* packet position */
    uint32_t w, h;
    uint32_t segment;
    int32_t next;                /* the next tile with the same hash, or -1 */
    /* The latest frame's only: where it stands on screen at alpha 0. */
    int32_t from_x, from_y;
    uint8_t kind;
} tile_t;

typedef struct {
    uint32_t word;               /* the offset command; for segment 0, the offset in effect */
    int32_t org_x, org_y;        /* the drawing area's top-left when it was set */
    int32_t rel_x, rel_y;        /* the offset from that corner */
    int32_t width, height;       /* the drawing area's size then */
    uint32_t first, count;       /* its sprites */
    uint32_t textured;
    /* The latest frame's only. */
    uint32_t reason;
    int32_t from_x, from_y;      /* the offset from the corner at alpha 0 */
} segment_t;

typedef struct {
    tile_t *tiles;
    segment_t *segments;
    size_t tile_count, tile_capacity, segment_count, segment_capacity;
    int32_t *heads;              /* hash of tile keys to the first tile */
    size_t head_capacity;
    int valid, failed;
} frame_t;

struct frame_layer {
    frame_t frames[2];
    unsigned latest;             /* frames[latest] ended last; the other is being drawn */
    /* GPU state as the commands set it. */
    uint32_t page, offset;
    int32_t area_x, area_y, area_right, area_bottom;
    frame_layer_info_t info;
    uint32_t *votes;             /* per segment of the frame before: tiles that share a key with it */
    size_t vote_capacity;
    uint8_t *cover;              /* a drawing area's pixels, for the edge check */
    size_t cover_capacity;
};

static int grow(void **items, size_t *capacity, size_t needed, size_t size) {
    size_t n = *capacity ? *capacity : 64;
    void *p;
    if (needed <= *capacity) return 1;
    while (n < needed) n *= 2;
    if (!(p = realloc(*items, n * size))) return 0;
    *items = p; *capacity = n;
    return 1;
}

static int32_t extend11(uint32_t v) { return (int32_t)(v << 21) >> 21; }

static frame_t *building(frame_layer_t *l) { return &l->frames[l->latest ^ 1]; }

/* A new frame starts with one segment: what it draws under the offset
 * already in effect. */
static void begin(frame_layer_t *l) {
    frame_t *f = building(l);
    segment_t *s;
    f->tile_count = f->segment_count = 0;
    f->valid = 0; f->failed = 0;
    if (!grow((void **)&f->segments, &f->segment_capacity, 1, sizeof(*f->segments))) { f->failed = 1; return; }
    s = &f->segments[f->segment_count++];
    memset(s, 0, sizeof(*s));
    s->word = l->offset;
    s->org_x = l->area_x; s->org_y = l->area_y;
    s->rel_x = extend11(l->offset) - l->area_x; s->rel_y = extend11(l->offset >> 11) - l->area_y;
    s->width = l->area_right - l->area_x + 1; s->height = l->area_bottom - l->area_y + 1;
}

frame_layer_t *frame_layer_create(void) {
    frame_layer_t *l = calloc(1, sizeof(*l));
    if (l) frame_layer_reset(l);
    return l;
}

void frame_layer_destroy(frame_layer_t *l) {
    unsigned i;
    if (!l) return;
    for (i = 0; i < 2; ++i) { free(l->frames[i].tiles); free(l->frames[i].segments); free(l->frames[i].heads); }
    free(l->votes); free(l->cover);
    free(l);
}

void frame_layer_reset(frame_layer_t *l) {
    l->frames[0].valid = l->frames[1].valid = 0;
    l->page = 0; l->offset = 0;
    l->area_x = l->area_y = 0; l->area_right = 1023; l->area_bottom = 511;
    memset(&l->info, 0, sizeof(l->info));
    begin(l);
}

const frame_layer_info_t *frame_layer_info(const frame_layer_t *l) { return &l->info; }

/* The texture page a textured polygon sets: the high half of its second
 * texture coordinate word. */
static void polygon_page(frame_layer_t *l, const uint32_t *w, unsigned count) {
    unsigned op = w[0] >> 24, k = 1, i;
    if (!(op & 4)) return;
    for (i = 0; i < 2; ++i) {
        if (i && (op & 16)) ++k;
        k += 2;
    }
    if (k - 1 < count) l->page = (w[k - 1] >> 16) & 0x9ffu;
}

int frame_layer_command(frame_layer_t *l, const uint32_t *w, unsigned count) {
    frame_t *f = building(l);
    unsigned op;
    if (!count) return 1;
    op = w[0] >> 24;
    if (op >= 0x20 && op < 0x40) polygon_page(l, w, count);
    else if (op == 0xe1) l->page = w[0] & 0x9ffu;
    else if (op == 0xe3) { l->area_x = (int32_t)(w[0] & 1023u); l->area_y = (int32_t)((w[0] >> 10) & 511u); }
    else if (op == 0xe4) { l->area_right = (int32_t)(w[0] & 1023u); l->area_bottom = (int32_t)((w[0] >> 10) & 511u); }
    else if (op == 0xe5) {
        segment_t *s;
        l->offset = w[0] & 0x3fffffu;
        if (f->failed || !grow((void **)&f->segments, &f->segment_capacity, f->segment_count + 1, sizeof(*f->segments))) {
            f->failed = 1; return 0;
        }
        s = &f->segments[f->segment_count++];
        memset(s, 0, sizeof(*s));
        s->word = w[0];
        s->org_x = l->area_x; s->org_y = l->area_y;
        s->rel_x = extend11(w[0]) - l->area_x; s->rel_y = extend11(w[0] >> 11) - l->area_y;
        s->width = l->area_right - l->area_x + 1; s->height = l->area_bottom - l->area_y + 1;
        s->first = (uint32_t)f->tile_count;
    } else if (op >= 0x60 && op < 0x80) {
        tile_t *t;
        unsigned k = (op & 4) ? 3 : 2;
        if (f->failed || count < 2 || !grow((void **)&f->tiles, &f->tile_capacity, f->tile_count + 1, sizeof(*f->tiles))) {
            f->failed = 1; return 0;
        }
        t = &f->tiles[f->tile_count++];
        memset(t, 0, sizeof(*t));
        t->x = extend11(w[1]); t->y = extend11(w[1] >> 16);
        switch ((op >> 3) & 3) {
        case 0: t->w = k < count ? w[k] & 1023u : 0; t->h = k < count ? (w[k] >> 16) & 511u : 0; break;
        case 1: t->w = t->h = 1; break;
        case 2: t->w = t->h = 8; break;
        default: t->w = t->h = 16; break;
        }
        /* What it draws: the semi-transparency mode and the colour change
         * how it looks (a fade), not which tile it is. */
        if ((op & 4) && count > 2)
            t->key = (uint64_t)1 << 63 | (uint64_t)(w[2] & 0xffffffffu) | (uint64_t)(l->page & 0x99fu) << 32 |
                     (uint64_t)t->w << 44 | (uint64_t)t->h << 54;
        t->segment = (uint32_t)f->segment_count - 1;
        ++f->segments[t->segment].count;
        if (t->key) ++f->segments[t->segment].textured;
    }
    return 1;
}

static size_t hash_of(uint64_t key, size_t capacity) {
    return (size_t)(((key * UINT64_C(0x9e3779b97f4a7c15)) >> 32) & (capacity - 1));
}

static int index_keys(frame_t *f) {
    size_t n = 64, i;
    while (n < f->tile_count * 2) n *= 2;
    if (n > f->head_capacity) {
        int32_t *h = realloc(f->heads, n * sizeof(*h));
        if (!h) return 0;
        f->heads = h; f->head_capacity = n;
    }
    memset(f->heads, 0xff, f->head_capacity * sizeof(*f->heads));
    for (i = f->tile_count; i-- > 0;) {
        size_t at;
        if (!f->tiles[i].key) continue;
        at = hash_of(f->tiles[i].key, f->head_capacity);
        f->tiles[i].next = f->heads[at];
        f->heads[at] = (int32_t)i;
    }
    return 1;
}

static int32_t first_with(const frame_t *f, uint64_t key) {
    int32_t i = f->heads[hash_of(key, f->head_capacity)];
    while (i >= 0 && f->tiles[i].key != key) i = f->tiles[i].next;
    return i;
}

static int32_t next_with(const frame_t *f, int32_t i) {
    uint64_t key = f->tiles[i].key;
    for (i = f->tiles[i].next; i >= 0 && f->tiles[i].key != key; i = f->tiles[i].next) {}
    return i;
}

/* Integer interpolation that gives each end exactly, as frame_blend does. */
static int32_t between(int32_t a, int32_t b, uint32_t num, uint32_t den) {
    int64_t d = ((int64_t)b - a) * num;
    return (int32_t)(a + (d >= 0 ? (d + den / 2) / den : -((-d + den / 2) / (int64_t)den)));
}

typedef struct {
    int32_t dx, dy;
    uint32_t count;
} motion_t;

static void vote(motion_t *m, unsigned *n, unsigned size, int32_t dx, int32_t dy) {
    unsigned i;
    for (i = 0; i < *n; ++i)
        if (m[i].dx == dx && m[i].dy == dy) { ++m[i].count; return; }
    if (*n < size) { m[*n].dx = dx; m[*n].dy = dy; m[*n].count = 1; ++*n; }
}

/* Paints the segment's sprites at alpha into one bit of the cover map. */
static void paint(frame_layer_t *l, const frame_t *f, const segment_t *s, uint32_t num, uint32_t den, uint8_t bit) {
    uint32_t i;
    for (i = s->first; i < s->first + s->count; ++i) {
        const tile_t *t = &f->tiles[i];
        int32_t x = between(t->from_x, t->x + s->rel_x, num, den), y = between(t->from_y, t->y + s->rel_y, num, den);
        int32_t x0 = x < 0 ? 0 : x, y0 = y < 0 ? 0 : y, x1 = x + (int32_t)t->w, y1 = y + (int32_t)t->h, j;
        if (x1 > s->width) x1 = s->width;
        if (y1 > s->height) y1 = s->height;
        for (j = y0; j < y1; ++j) if (x0 < x1) {
            uint8_t *row = l->cover + (size_t)j * (size_t)s->width;
            int32_t k;
            for (k = x0; k < x1; ++k) row[k] |= bit;
        }
    }
}

/* Moving the segment must not uncover what the newer frame covers in its
 * drawing area: the game draws only the tiles its own scroll shows. A rigid
 * move uncovers most at alpha 0; the quarters catch a hole between tiles. */
static int uncovers(frame_layer_t *l, const frame_t *f, const segment_t *s) {
    size_t size, k;
    uint32_t q;
    if (s->width <= 0 || s->height <= 0 || s->width > 1024 || s->height > 512) return 0;
    size = (size_t)s->width * (size_t)s->height;
    if (!grow((void **)&l->cover, &l->cover_capacity, size, 1)) return 1;
    for (q = 0; q < 4; ++q) {
        memset(l->cover, 0, size);
        paint(l, f, s, 1, 1, 1);
        paint(l, f, s, q, 4, 2);
        for (k = 0; k < size; ++k) if (l->cover[k] == 1) return 1;
    }
    return 0;
}

/* One segment of the newer frame against the frame before. */
static uint32_t move_segment(frame_layer_t *l, frame_t *f, segment_t *s, const frame_t *p, uint32_t *paired,
                             uint32_t *unpaired, uint32_t *disagree, uint32_t *untextured, int32_t *dx, int32_t *dy,
                             uint32_t *layers) {
    motion_t found[64], groups[FRAME_LAYER_GROUPS];
    unsigned nfound = 0, ngroups = 0, g;
    uint32_t i, best = 0, moving = 0;
    const segment_t *ps;
    int32_t off_x, off_y;
    *paired = *unpaired = *disagree = *untextured = 0; *dx = *dy = 0; *layers = 0;
    for (i = s->first; i < s->first + s->count; ++i) {
        tile_t *t = &f->tiles[i];
        t->from_x = t->x + s->rel_x; t->from_y = t->y + s->rel_y; t->kind = KIND_HELD;
        if (!t->key) ++*untextured;
    }
    if (s->textured < FRAME_LAYER_TILES) return FRAME_LAYER_FEW;
    /* The segment before that shares most of its tiles. */
    if (!grow((void **)&l->votes, &l->vote_capacity, p->segment_count, sizeof(*l->votes))) return FRAME_LAYER_CHANGED;
    memset(l->votes, 0, p->segment_count * sizeof(*l->votes));
    for (i = s->first; i < s->first + s->count; ++i) {
        int32_t j;
        uint32_t last = UINT32_MAX;
        if (!f->tiles[i].key) continue;
        for (j = first_with(p, f->tiles[i].key); j >= 0; j = next_with(p, j))
            if (p->tiles[j].segment != last) { ++l->votes[p->tiles[j].segment]; last = p->tiles[j].segment; }
    }
    for (i = 1; i < p->segment_count; ++i) if (l->votes[i] > l->votes[best]) best = i;
    if (l->votes[best] * 2 < s->textured) return FRAME_LAYER_CHANGED;
    ps = &p->segments[best];
    off_x = s->rel_x - ps->rel_x; off_y = s->rel_y - ps->rel_y;
    /* The moves on screen of tiles whose key appears once in each. */
    for (i = s->first; i < s->first + s->count; ++i) {
        const tile_t *t = &f->tiles[i];
        int32_t j, only = -1, n = 0, k;
        if (!t->key) continue;
        for (j = first_with(p, t->key); j >= 0; j = next_with(p, j))
            if (p->tiles[j].segment == best) { only = j; ++n; }
        if (n != 1) continue;
        for (k = first_with(f, t->key); k >= 0; k = next_with(f, k))
            if (k != (int32_t)i && f->tiles[k].segment == t->segment) break;
        if (k >= 0) continue;
        vote(found, &nfound, 64, t->x + s->rel_x - p->tiles[only].x - ps->rel_x, t->y + s->rel_y - p->tiles[only].y - ps->rel_y);
    }
    /* The main layer is the move most share, the offset's own on a tie or
     * when no key is unique; others that enough share are layers of their own. */
    groups[0].dx = off_x; groups[0].dy = off_y; groups[0].count = 0;
    for (i = 0; i < nfound; ++i)
        if (found[i].count > groups[0].count ||
            (found[i].count == groups[0].count && found[i].dx == off_x && found[i].dy == off_y)) groups[0] = found[i];
    ngroups = 1;
    while (ngroups < FRAME_LAYER_GROUPS) {
        motion_t next = {0, 0, 0};
        for (i = 0; i < nfound; ++i) {
            for (g = 0; g < ngroups; ++g) if (found[i].dx == groups[g].dx && found[i].dy == groups[g].dy) break;
            if (g == ngroups && found[i].count > next.count) next = found[i];
        }
        if (next.count < FRAME_LAYER_PARALLAX) break;
        groups[ngroups++] = next;
    }
    /* Each tile: with the layer whose move finds its pair, with the main
     * layer if it has none, held if it has one that moved otherwise. */
    for (i = s->first; i < s->first + s->count; ++i) {
        tile_t *t = &f->tiles[i];
        int32_t sx = t->x + s->rel_x, sy = t->y + s->rel_y, j, k;
        unsigned before = 0, now = 0;
        if (!t->key) continue;
        for (g = 0; g < ngroups; ++g) {
            for (j = first_with(p, t->key); j >= 0; j = next_with(p, j)) {
                const tile_t *o = &p->tiles[j];
                if (o->segment != best) continue;
                if (!g) ++before;
                if (o->x + ps->rel_x == sx - groups[g].dx && o->y + ps->rel_y == sy - groups[g].dy) break;
            }
            if (j >= 0) break;
        }
        for (k = first_with(f, t->key); k >= 0; k = next_with(f, k)) now += f->tiles[k].segment == t->segment;
        if (g < ngroups) {
            t->kind = KIND_LAYER; t->from_x = sx - groups[g].dx; t->from_y = sy - groups[g].dy; ++*paired;
        } else if (before == 1 && now == 1) {
            /* Only a tile that is one of a kind in both frames is known to
             * have moved by itself; one of several alike may be another
             * coming in at the edge. */
            t->kind = KIND_DISAGREE; ++*disagree;
        } else {
            t->kind = KIND_UNPAIRED; t->from_x = sx - groups[0].dx; t->from_y = sy - groups[0].dy; ++*unpaired;
        }
    }
    *dx = groups[0].dx; *dy = groups[0].dy;
    for (g = 0; g < ngroups; ++g) if (groups[g].dx || groups[g].dy) ++moving;
    s->from_x = ps->rel_x; s->from_y = ps->rel_y;
    if (*disagree * 4 > *paired) return FRAME_LAYER_NOT_RIGID;
    /* Tiles that stand still while the offset moves were moved against it in
     * the picture: what else it draws is not known to follow either. */
    if (!moving) return FRAME_LAYER_STILL;
    if (uncovers(l, f, s)) return FRAME_LAYER_EDGE;
    *layers = ngroups;
    return FRAME_LAYER_MOVED;
}

/* A segment that does not move draws where the newer frame has it. */
static void hold(frame_t *f, segment_t *s) {
    uint32_t i;
    s->from_x = s->rel_x; s->from_y = s->rel_y;
    for (i = s->first; i < s->first + s->count; ++i) {
        f->tiles[i].from_x = f->tiles[i].x + s->rel_x; f->tiles[i].from_y = f->tiles[i].y + s->rel_y;
    }
}

int frame_layer_end(frame_layer_t *l, int history) {
    frame_t *f = building(l), *p = &l->frames[l->latest];
    frame_layer_info_t *info = &l->info;
    uint64_t number = info->number + 1;
    uint32_t i, main = 0;
    int ok = !f->failed;
    memset(info, 0, sizeof(*info));
    info->number = number;
    info->segments = f->segment_count ? (uint32_t)f->segment_count - 1 : 0;
    info->reason = FRAME_LAYER_NO_HISTORY;
    if (ok && !index_keys(f)) ok = 0;
    for (i = 0; i < f->segment_count; ++i) {
        segment_t *s = &f->segments[i];
        if (s->textured > f->segments[main].textured) main = i;
        s->reason = FRAME_LAYER_NO_HISTORY;
        hold(f, s);
    }
    if (ok && history && p->valid && !p->failed) {
        for (i = 0; i < f->segment_count; ++i) {
            segment_t *s = &f->segments[i];
            uint32_t paired, unpaired, disagree, untextured, layers;
            int32_t dx, dy;
            s->reason = move_segment(l, f, s, p, &paired, &unpaired, &disagree, &untextured, &dx, &dy, &layers);
            if (s->reason != FRAME_LAYER_MOVED) hold(f, s);
            else ++info->moving_segments;
            if (i == main) {
                info->reason = s->reason; info->dx = dx; info->dy = dy; info->layers = layers;
                info->offset_dx = s->rel_x - s->from_x; info->offset_dy = s->rel_y - s->from_y;
                info->paired = paired; info->unpaired = unpaired; info->disagree = disagree; info->untextured = untextured;
            }
        }
        /* An offset command that sets again the offset of a segment that
         * moves, with nothing drawn under it yet, moves with it: what is
         * drawn after it in the frame is in the same picture. */
        for (i = 0; i < f->segment_count; ++i) {
            segment_t *s = &f->segments[i];
            uint32_t k;
            if (s->reason == FRAME_LAYER_MOVED || s->count) continue;
            for (k = 0; k < f->segment_count; ++k) {
                const segment_t *o = &f->segments[k];
                if (o->reason == FRAME_LAYER_MOVED && o->rel_x == s->rel_x && o->rel_y == s->rel_y) {
                    s->reason = FRAME_LAYER_MOVED; s->from_x = o->from_x; s->from_y = o->from_y;
                    break;
                }
            }
        }
    }
    info->tiles = f->segments ? f->segments[main].textured : 0;
    f->valid = ok;
    l->latest ^= 1;
    begin(l);
    return ok;
}

static const segment_t *segment_at(const frame_layer_t *l, uint32_t segment) {
    const frame_t *f = &l->frames[l->latest];
    if (!f->valid || segment >= f->segment_count || f->segments[segment].reason != FRAME_LAYER_MOVED) return NULL;
    return &f->segments[segment];
}

int frame_layer_moves(const frame_layer_t *l, uint32_t segment) { return segment_at(l, segment) != NULL; }

int frame_layer_offset(const frame_layer_t *l, uint32_t index, uint32_t num, uint32_t den, uint32_t *word) {
    const segment_t *s = segment_at(l, index + 1);
    int32_t x, y;
    if (!s || !den || num >= den) return 0;
    x = s->org_x + between(s->from_x, s->rel_x, num, den);
    y = s->org_y + between(s->from_y, s->rel_y, num, den);
    *word = 0xe5000000u | ((uint32_t)x & 0x7ffu) | ((uint32_t)y & 0x7ffu) << 11;
    return *word != s->word;
}

int frame_layer_sprite(const frame_layer_t *l, uint32_t index, uint32_t num, uint32_t den, uint32_t *xy) {
    const frame_t *f = &l->frames[l->latest];
    const tile_t *t;
    const segment_t *s;
    int32_t x, y;
    if (!f->valid || index >= f->tile_count || !den || num >= den) return 0;
    t = &f->tiles[index];
    if (!(s = segment_at(l, t->segment))) return 0;
    x = between(t->from_x, t->x + s->rel_x, num, den) - between(s->from_x, s->rel_x, num, den);
    y = between(t->from_y, t->y + s->rel_y, num, den) - between(s->from_y, s->rel_y, num, den);
    if (x != t->x || y != t->y) *xy = ((uint32_t)x & 0xffffu) | ((uint32_t)y & 0xffffu) << 16;
    return t->kind == KIND_LAYER || t->kind == KIND_UNPAIRED;
}

void frame_layer_shift(const frame_layer_t *l, uint32_t num, uint32_t den, int32_t *x, int32_t *y) {
    const frame_t *f = &l->frames[l->latest];
    uint32_t i, main = 0;
    const segment_t *s;
    *x = *y = 0;
    if (!f->valid || !den || num >= den) return;
    for (i = 0; i < f->segment_count; ++i) if (f->segments[i].textured > f->segments[main].textured) main = i;
    if (!(s = segment_at(l, main))) return;
    *x = between(s->from_x, s->rel_x, num, den) - s->rel_x;
    *y = between(s->from_y, s->rel_y, num, den) - s->rel_y;
}
