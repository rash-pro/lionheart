/* Draws the frames between a game's frames from a recording (--frame-record,
 * docs/frame-blend.md) through the software rasterizer, and checks and
 * measures them:
 *   inbetween RECORD [--from V] [--until V] [--out DIR] [--canvas WxH] [--sheet V] [--frames]
 *             [--causes] [--packets V] [--straight]
 * Every VBlank of the recording is replayed and must show what the game showed.
 * Every game frame is drawn again with its polygons where they stand at the end
 * of its interval (alpha 1), which must give the game's picture pixel for
 * pixel, and at its start (alpha 0), which is compared with the picture before
 * it: what differs there still moves only at the game's rate. Between from and
 * until, --out writes off.rgb (the game's pictures, one per VBlank) and on.rgb
 * (the blended ones) as raw 24-bit frames of the canvas size, and heat.ppm:
 * where pixels changed at game frames (red) or between them (green). --sheet
 * writes the pictures of the first blended game frame from VBlank V on, and
 * --frames prints what blending gave each game frame. --causes says what drew
 * the pixels that still jump and counts holes in the blended pictures;
 * --packets V lists the packets behind them for one game frame. --straight
 * moves every corner on a straight line, as before parts turned.
 * The report goes to stdout; the exit status is 1 if a check failed. */
#include "../../runtime/gpu.c"
#include "frame_blend.h"
#include "frame_record.h"
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { WIDTH = 1024, HEIGHT = 512 };

typedef struct {
    uint8_t *data;
    size_t size, capacity;
} bytes_t;

static int append(bytes_t *b, const void *p, size_t n) {
    if (b->size + n > b->capacity) {
        size_t c = b->capacity ? b->capacity * 2 : 1 << 20;
        uint8_t *d;
        while (c < b->size + n) c *= 2;
        if (!(d = realloc(b->data, c))) return 0;
        b->data = d; b->capacity = c;
    }
    memcpy(b->data + b->size, p, n);
    b->size += n;
    return 1;
}

static uint32_t le32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static uint64_t le64(const uint8_t *p) { return le32(p) | (uint64_t)le32(p + 4) << 32; }
static void put_le32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); }

/* A GPU that counts the polygons it draws, as the identity service does. */
typedef struct {
    gpu_t *gpu;
    uint64_t serial;
} counted_t;
static void count_polygon(void *context, unsigned opcode, const uint32_t *packet) {
    (void)opcode; (void)packet;
    ++((counted_t *)context)->serial;
}
static void ignore_start(void *context, uint32_t start) { (void)context; (void)start; }
static void ignore_vblank(void *context) { (void)context; }
static const gpu_observer_t counter = {ignore_start, ignore_vblank, count_polygon};

/* A GPU state with the serial of its next polygon. */
typedef struct {
    bytes_t state;
    uint64_t serial;
    int valid;
} checkpoint_t;

static int save(checkpoint_t *k, counted_t *g) {
    ss_io io;
    memset(&io, 0, sizeof(io));
    ss_gpu(&io, g->gpu);
    if (io.failed) { free(io.data); return 0; }
    free(k->state.data);
    k->state.data = io.data; k->state.size = io.size; k->state.capacity = io.capacity;
    k->serial = g->serial; k->valid = 1;
    return 1;
}

static int restore(counted_t *g, const uint8_t *state, size_t size, uint64_t serial) {
    ss_io io;
    memset(&io, 0, sizeof(io));
    io.data = (uint8_t *)state; io.size = size; io.reading = 1; io.version = 1;
    ss_gpu(&io, g->gpu);
    g->serial = serial;
    return !io.failed && io.pos == size;
}

/* The display area as the host presents it, centred on a canvas. */
static void expand(uint16_t v, uint8_t *o) {
    unsigned i;
    for (i = 0; i < 3; ++i) { unsigned x = (v >> (i * 5)) & 31; o[i] = (uint8_t)((x << 3) | (x >> 2)); }
}
static void picture(gpu_t *g, uint8_t *out, int cw, int ch) {
    int x, y, w, h, rgb24, enabled, i, j;
    const uint16_t *vram = gpu_vram(g);
    memset(out, 0, (size_t)cw * ch * 3);
    gpu_display(g, &x, &y, &w, &h, &rgb24, &enabled);
    if (!enabled) return;
    for (j = 0; j < h; ++j) {
        int oy = j + (ch - h) / 2;
        if (oy < 0 || oy >= ch) continue;
        for (i = 0; i < w; ++i) {
            int ox = i + (cw - w) / 2;
            uint8_t *o;
            unsigned row = ((unsigned)(y + j) & 511) * 1024;
            if (ox < 0 || ox >= cw) continue;
            o = out + ((size_t)oy * cw + ox) * 3;
            if (rgb24) {
                unsigned k, at = (unsigned)x * 2 + (unsigned)i * 3;
                for (k = 0; k < 3; ++k) { unsigned byte = (at + k) & 2047; o[k] = (uint8_t)(vram[row + byte / 2] >> ((byte & 1) * 8)); }
            } else expand(vram[row + ((unsigned)(x + i) & 1023)], o);
        }
    }
}

/* What drew a pixel, for the causes of what still changes only at game
 * frames (--causes). 3D polygons are those with a corner at a projection;
 * a held one is classed by the first reason that applies, in this order. */
enum {
    CAUSE_NONE,           /* nothing drew it during the replay */
    CAUSE_MOVED,          /* a 3D polygon that moves, its look changed (colour, UV, order, what lies under it) */
    CAUSE_MOVED_TEXTURE,  /* a 3D polygon that moves, its texture or palette rewritten in the frame */
    CAUSE_NO_HISTORY,     /* a 3D polygon with a corner from a frame without history */
    CAUSE_CPU_CORNER,     /* a 3D polygon with a corner no projection made */
    CAUSE_UNPAIRED,       /* a 3D polygon with a projection that has no pair */
    CAUSE_AMBIGUOUS,      /* a 3D polygon with a corner where projections were apart, or split across frames */
    CAUSE_BLEND,          /* a 3D polygon held by blending: next to a held one, would stretch, or its ends */
    CAUSE_FLAT,           /* a polygon with no corner at a projection: 2D, or corners the CPU made */
    CAUSE_SPRITE,         /* rectangles and sprites */
    CAUSE_LINE,
    CAUSE_FILL,
    CAUSE_TRANSFER,       /* uploads and copies into the picture */
    CAUSE_AFTER,          /* drawn after the display start changed: what the shown VBlank adds */
    CAUSE_FRAME,          /* the whole frame is not blended (a reset or a cut) */
    CAUSES
};
static const char *const cause_names[CAUSES] = {
    "nothing-drawn", "3d-moved-look-changed", "3d-moved-texture-rewritten", "3d-no-history", "3d-corner-without-projection",
    "3d-unpaired", "3d-ambiguous-or-split", "3d-held-by-blending", "polygon-without-projection", "sprite", "line",
    "fill", "transfer", "after-display-change", "frame-not-blended"};
static const uint8_t cause_colours[CAUSES][3] = {
    {128, 128, 128}, {255, 230, 0}, {255, 140, 0}, {255, 255, 255}, {255, 0, 0}, {0, 200, 255}, {60, 60, 255},
    {255, 0, 255}, {0, 210, 0}, {170, 255, 170}, {170, 90, 40}, {90, 90, 90}, {255, 160, 210}, {0, 140, 140},
    {200, 200, 200}};

/* A packet that drew into the picture, for --packets. */
typedef struct {
    uint32_t words[12];
    uint64_t serial;
    unsigned count, cause, mask;
    int x0, y0, x1, y1;       /* the picture pixels it changed */
} drawer_t;

typedef struct {
    int x, y, w, h;           /* the display area in VRAM */
    uint16_t *shadow;         /* its pixels as they stood before the packet now drawn */
    uint8_t *cause;           /* per pixel of it: what drew it last */
    uint32_t *drawer;         /* per pixel: that packet, 1 + its index in drawers, or 0 */
    drawer_t *drawers;
    size_t drawer_count, drawer_capacity;
    int describe;             /* keep drawers */
    /* VRAM that transfers, copies and fills changed during the replay */
    int rects[64][4];
    unsigned rect_count;
    /* A transfer in progress and what its area held before */
    int tx, ty, tw, th;
    uint16_t *before;
} trace_t;

/* Replay. */
typedef struct {
    const frame_blend_t *blend;
    uint32_t num, den;
    frame_blend_check_t check;
    uint64_t split;          /* polygon packets cut between chunks, drawn as recorded */
    trace_t *trace;          /* or NULL */
    const frame_identity_frame_t *frame;
    /* Why the frame blended has no history, and the one before it: a corner
     * moves only if the frame its projection belongs to has one. */
    unsigned reason, reason_before;
    int after;               /* past the display-start change that ends the frame */
} blend_at_t;

/* The trace starts from the display area of the picture the replay ends on,
 * as it stands in the GPU replayed from. */
static void trace_begin(trace_t *t, gpu_t *g) {
    int j;
    for (j = 0; j < t->h; ++j) {
        int i;
        for (i = 0; i < t->w; ++i) t->shadow[j * t->w + i] = gpu_vram(g)[((t->y + j) & 511) * 1024 + ((t->x + i) & 1023)];
    }
    memset(t->cause, CAUSE_NONE, (size_t)t->w * t->h);
    memset(t->drawer, 0, (size_t)t->w * t->h * sizeof(*t->drawer));
    t->drawer_count = 0; t->rect_count = 0; t->tw = 0;
}

static void copy_area(const gpu_t *g, uint16_t *to, int x, int y, int w, int h) {
    int i, j;
    for (j = 0; j < h; ++j) for (i = 0; i < w; ++i) to[j * w + i] = g->raster.vram[((y + j) & 511) * 1024 + ((x + i) & 1023)];
}

/* Where an area of VRAM changed since copy_area took it. */
static void note_change(trace_t *t, const gpu_t *g, const uint16_t *was, int x, int y, int w, int h) {
    int i, j, x0 = w, y0 = h, x1 = -1, y1 = -1;
    for (j = 0; j < h; ++j)
        for (i = 0; i < w; ++i)
            if (g->raster.vram[((y + j) & 511) * 1024 + ((x + i) & 1023)] != was[j * w + i]) {
                if (i < x0) x0 = i;
                if (i > x1) x1 = i;
                if (j < y0) y0 = j;
                if (j > y1) y1 = j;
            }
    if (x1 < 0 || t->rect_count >= 64) return;
    t->rects[t->rect_count][0] = x + x0; t->rects[t->rect_count][1] = y + y0;
    t->rects[t->rect_count][2] = x + x1; t->rects[t->rect_count][3] = y + y1;
    ++t->rect_count;
}

static int rewritten(const trace_t *t, int x0, int y0, int x1, int y1) {
    unsigned i;
    for (i = 0; i < t->rect_count; ++i)
        if (t->rects[i][0] <= x1 && x0 <= t->rects[i][2] && t->rects[i][1] <= y1 && y0 <= t->rects[i][3]) return 1;
    return 0;
}

/* Whether the texture or palette a textured polygon samples was rewritten. */
static int texture_rewritten(const trace_t *t, const uint32_t *w, unsigned op) {
    unsigned corners = (op & 8) ? 4 : 3, i, k = 1, umin = 255, umax = 0, vmin = 255, vmax = 0, page = 0, clut = 0, depth;
    int x;
    if (!(op & 4)) return 0;
    for (i = 0; i < corners; ++i) {
        unsigned u, v;
        if (i && (op & 16)) ++k;
        ++k;
        u = w[k] & 255; v = (w[k] >> 8) & 255;
        if (!i) clut = w[k] >> 16;
        if (i == 1) page = (w[k] >> 16) & 0xffffu;
        if (u < umin) umin = u;
        if (u > umax) umax = u;
        if (v < vmin) vmin = v;
        if (v > vmax) vmax = v;
        ++k;
    }
    depth = (page >> 7) & 3;
    x = (int)(page & 15) * 64;
    if (rewritten(t, x + (int)(depth == 0 ? umin / 4 : depth == 1 ? umin / 2 : umin), (int)((page >> 4) & 1) * 256 + (int)vmin,
                  x + (int)(depth == 0 ? umax / 4 : depth == 1 ? umax / 2 : umax), (int)((page >> 4) & 1) * 256 + (int)vmax))
        return 1;
    return depth < 2 && rewritten(t, (int)(clut & 63) * 16, (int)((clut >> 6) & 511),
                                  (int)(clut & 63) * 16 + (depth ? 255 : 15), (int)((clut >> 6) & 511));
}

static unsigned polygon_cause(const blend_at_t *at, uint64_t serial, unsigned op, unsigned mask, const uint32_t *w) {
    const frame_identity_frame_t *f = at->frame;
    const frame_identity_polygon_t *q;
    unsigned i, three_d = 0, cpu = 0, unpaired = 0, ambiguous = 0, history = 0;
    unsigned reset = 0;
    if (!f || serial < f->first_polygon || serial - f->first_polygon >= f->polygon_count) return CAUSE_FLAT;
    q = &f->polygons[serial - f->first_polygon];
    for (i = 0; i < q->corners; ++i) {
        unsigned s = q->state[i];
        if (s == FRAME_IDENTITY_NO_PROJECTION) { ++cpu; continue; }
        ++three_d;
        if (s == FRAME_IDENTITY_LINKED && (q->age[i] > 1 || (q->age[i] ? at->reason_before : at->reason) != FRAME_BLEND_KEPT))
            ++reset;
        if (s == FRAME_IDENTITY_UNPAIRED) ++unpaired;
        else if (s == FRAME_IDENTITY_AMBIGUOUS || s == FRAME_IDENTITY_SPLIT) ++ambiguous;
        else if (s == FRAME_IDENTITY_NO_HISTORY || s == FRAME_IDENTITY_EXCLUDED_CORNER) ++history;
    }
    if (!three_d) return CAUSE_FLAT;
    if (mask == (1u << q->corners) - 1) return texture_rewritten(at->trace, w, op) ? CAUSE_MOVED_TEXTURE : CAUSE_MOVED;
    if (history || reset) return CAUSE_NO_HISTORY;
    if (cpu) return CAUSE_CPU_CORNER;
    if (unpaired) return CAUSE_UNPAIRED;
    if (ambiguous) return CAUSE_AMBIGUOUS;
    return CAUSE_BLEND;
}

static int grow_drawers(trace_t *t) {
    if (t->drawer_count == t->drawer_capacity) {
        size_t n = t->drawer_capacity ? t->drawer_capacity * 2 : 4096;
        drawer_t *d = realloc(t->drawers, n * sizeof(*d));
        if (!d) return 0;
        t->drawers = d; t->drawer_capacity = n;
    }
    return 1;
}

/* Words to the GPU; with a trace, what they change in the picture is
 * credited to them. */
static void draw(counted_t *c, blend_at_t *at, const uint8_t *p, unsigned n, unsigned cause, unsigned mask) {
    gpu_t *g = c->gpu;
    trace_t *t = at ? at->trace : NULL;
    unsigned op = g->transfer == 1 || g->count || g->polyline ? 0x100 : p[3];
    uint64_t serial = c->serial;
    int fx = 0, fy = 0, fw = 0, fh = 0, j;
    uint16_t *was = NULL;
    size_t index = 0;
    if (!t) { gpu_gp0_words(g, p, n); return; }
    if (at->after) cause = CAUSE_AFTER;
    if (op == 0x02 || (op >= 0x80 && op < 0xa0)) {
        /* A fill or copy: what it changes outside the picture may be a texture. */
        uint32_t w1 = le32(p + 4), w2 = le32(p + 8);
        if (op == 0x02) { fx = (int)(w1 & 0x3f0); fy = (int)((w1 >> 16) & 511); fw = (int)(((w2 & 1023) + 15) & ~15u); fh = (int)((w2 >> 16) & 511); }
        else { fx = (int)(w2 & 1023); fy = (int)((w2 >> 16) & 511); fw = (int)width(le32(p + 12)); fh = (int)height(le32(p + 12)); }
        if ((size_t)fw * fh <= (size_t)1024 * 512) { was = t->before; copy_area(g, was, fx, fy, fw, fh); }
    }
    if (t->describe && cause != CAUSE_NONE) {
        drawer_t *d;
        if (!grow_drawers(t)) { gpu_gp0_words(g, p, n); return; }
        d = &t->drawers[t->drawer_count];
        memset(d, 0, sizeof(*d));
        d->count = n < 12 ? n : 12;
        for (j = 0; j < (int)d->count; ++j) d->words[j] = le32(p + j * 4);
        d->serial = serial; d->cause = cause; d->mask = mask;
        d->x0 = d->y0 = INT32_MAX; d->x1 = d->y1 = -1;
        index = ++t->drawer_count;
    }
    gpu_gp0_words(g, p, n);
    if (was) note_change(t, g, was, fx, fy, fw, fh);
    if (op >= 0xa0 && op < 0xc0 && g->transfer == 1) {
        t->tx = (int)g->tx; t->ty = (int)g->ty; t->tw = (int)g->tw; t->th = (int)g->th;
        copy_area(g, t->before, t->tx, t->ty, t->tw, t->th);
    } else if (op == 0x100 && t->tw && g->transfer != 1) {
        note_change(t, g, t->before, t->tx, t->ty, t->tw, t->th);
        t->tw = 0;
    }
    if (cause == CAUSE_NONE) return;
    for (j = 0; j < t->h; ++j) {
        const uint16_t *row = &g->raster.vram[((t->y + j) & 511) * 1024];
        uint16_t *s = &t->shadow[j * t->w];
        int i;
        if (t->x + t->w <= 1024 && !memcmp(row + t->x, s, (size_t)t->w * 2)) continue;
        for (i = 0; i < t->w; ++i) {
            uint16_t v = row[(t->x + i) & 1023];
            if (v == s[i]) continue;
            s[i] = v;
            t->cause[j * t->w + i] = (uint8_t)cause;
            t->drawer[j * t->w + i] = (uint32_t)index;
            if (index) {
                drawer_t *d = &t->drawers[index - 1];
                if (i < d->x0) d->x0 = i;
                if (i > d->x1) d->x1 = i;
                if (j < d->y0) d->y0 = j;
                if (j > d->y1) d->y1 = j;
            }
        }
    }
}

/* The cause of words that are not a whole packet, by the packet they belong to. */
static unsigned cause_of(unsigned op) {
    if (op >= 0x20 && op < 0x40) return CAUSE_FLAT;
    if (op >= 0x40 && op < 0x60) return CAUSE_LINE;
    if (op >= 0x60 && op < 0x80) return CAUSE_SPRITE;
    if (op >= 0x80 && op < 0xc0) return CAUSE_TRANSFER;
    if (op == 0x02) return CAUSE_FILL;
    return CAUSE_NONE;
}

/* GP0 words, with the polygons of the blended frame where blending puts them. */
static void feed_gp0(counted_t *c, const uint8_t *p, unsigned words, blend_at_t *at) {
    gpu_t *g = c->gpu;
    while (words) {
        unsigned n = 1, op, need, cause = CAUSE_NONE;
        if (g->transfer == 1) {
            unsigned left = g->tw * g->th - g->cursor;
            n = (left + 1) / 2;
            if (n > words) n = words;
            if (!n) n = 1;
            cause = CAUSE_TRANSFER;
        } else if (g->polyline) cause = CAUSE_LINE;
        else if (g->count) cause = cause_of(g->opcode);
        else {
            op = p[3];
            need = packet_size(op);
            cause = cause_of(op);
            if (need <= words) {
                n = need;
                if (at && op >= 0x20 && op < 0x40) {
                    uint8_t packet[12 * 4];
                    uint32_t xy[4], w[12];
                    unsigned corners = (op & 8) ? 4 : 3, i, k = 1, mask;
                    memcpy(packet, p, n * 4);
                    mask = frame_blend_polygon(at->blend, c->serial, at->num, at->den, xy, &at->check);
                    for (i = 0; i < corners; ++i) {
                        if (i && (op & 16)) ++k;
                        if (mask & (1u << i)) put_le32(packet + k * 4, xy[i]);
                        ++k;
                        if (op & 4) ++k;
                    }
                    if (at->trace) {
                        for (i = 0; i < n; ++i) w[i] = le32(packet + i * 4);
                        cause = polygon_cause(at, c->serial, op, mask, w);
                    }
                    draw(c, at, packet, n, cause, mask);
                    p += n * 4; words -= n;
                    continue;
                }
            } else if (at && op >= 0x20 && op < 0x40) ++at->split;
        }
        draw(c, at, p, n, cause, 0);
        p += n * 4; words -= n;
    }
}

/* One chunk of the recording, to a GPU. */
static void apply(counted_t *c, uint32_t tag, const uint8_t *p, size_t size, blend_at_t *at) {
    if (tag == FRAME_RECORD_GP0) feed_gp0(c, p, (unsigned)(size / 4), at);
    else if (tag == FRAME_RECORD_GP1 && size == 4) gpu_gp1(c->gpu, le32(p));
    else if (tag == FRAME_RECORD_READ) (void)gpu_read(c->gpu);
    else if (tag == FRAME_RECORD_VBLK) gpu_vblank(c->gpu);
}

/* Chunks since a checkpoint: tag, size, payload, one after another. */
typedef struct {
    bytes_t chunks;
    size_t mark;             /* where the next checkpoint's chunks begin */
} journal_t;

static int journal(journal_t *j, uint32_t tag, const uint8_t *p, size_t size) {
    uint8_t head[8];
    put_le32(head, tag); put_le32(head + 4, (uint32_t)size);
    return append(&j->chunks, head, 8) && append(&j->chunks, p, size);
}

/* The journal from checkpoint a. The chunks from the mark on come after the
 * display-start change that ends the blended frame. */
static void replay(counted_t *c, const journal_t *j, blend_at_t *at) {
    size_t i = 0;
    if (at && at->trace) trace_begin(at->trace, c->gpu);
    while (i + 8 <= j->chunks.size) {
        uint32_t tag = le32(j->chunks.data + i), size = le32(j->chunks.data + i + 4);
        if (at) at->after = i >= j->mark;
        apply(c, tag, j->chunks.data + i + 8, size, at);
        i += 8 + size;
    }
}

static int write_ppm(const char *dir, const char *name, const uint8_t *rgb, int w, int h) {
    char path[4096];
    FILE *f;
    int ok;
    snprintf(path, sizeof(path), "%s/%s", dir, name);
    if (!(f = fopen(path, "wb"))) return 0;
    ok = fprintf(f, "P6\n%d %d\n255\n", w, h) > 0 && fwrite(rgb, 1, (size_t)w * h * 3, f) == (size_t)w * h * 3;
    return fclose(f) == 0 && ok;
}

static const char *const reasons[FRAME_BLEND_REASONS] = {"kept", "no-history", "excluded", "scene", "cut"};

/* The trace follows the display area of the picture shown. */
static void trace_area(trace_t *t, gpu_t *shown) {
    int rgb24, enabled;
    gpu_display(shown, &t->x, &t->y, &t->w, &t->h, &rgb24, &enabled);
    if (t->w > WIDTH) t->w = WIDTH;
    if (t->h > HEIGHT) t->h = HEIGHT;
}

/* The trace's maps on the canvas, placed as picture() places the display area. */
static void to_canvas(const trace_t *t, uint8_t *cause, uint32_t *drawer, int cw, int ch) {
    int ox, oy;
    for (oy = 0; oy < ch; ++oy)
        for (ox = 0; ox < cw; ++ox) {
            int i = ox - (cw - t->w) / 2, j = oy - (ch - t->h) / 2, inside = i >= 0 && i < t->w && j >= 0 && j < t->h;
            cause[oy * cw + ox] = inside ? t->cause[j * t->w + i] : CAUSE_NONE;
            if (drawer) drawer[oy * cw + ox] = inside ? t->drawer[j * t->w + i] : 0;
        }
}

/* Why a pixel that changes at a game frame jumps: what alpha 0 draws there,
 * unless that moves or is the background, when it is what the picture before
 * had there (something held that left, or that a polygon moved over); a moving
 * polygon over a moving polygon is one whose look changed. */
static unsigned blamed(unsigned zero, unsigned before) {
    int open = zero == CAUSE_MOVED || zero == CAUSE_MOVED_TEXTURE || zero == CAUSE_FILL || zero == CAUSE_NONE;
    int held = before != CAUSE_MOVED && before != CAUSE_MOVED_TEXTURE && before != CAUSE_FILL && before != CAUSE_NONE;
    return open && held ? before : zero;
}

/* Renders with at least this many hole pixels are listed: a crack or a gap one can see. */
enum { HOLE = 16 };

static int three_d(unsigned cause) { return cause >= CAUSE_MOVED && cause <= CAUSE_BLEND; }

static int by_pixels(const void *a, const void *b) {
    const uint32_t *x = a, *y = b;
    return x[1] < y[1] ? 1 : x[1] > y[1] ? -1 : (x[0] > y[0]) - (x[0] < y[0]);
}

static const char *const states[] = {"linked", "no-projection", "split", "unpaired", "ambiguous", "no-history", "excluded"};

/* --packets: the packets that alpha 0 of one game frame draws where the
 * picture still jumps, the most pixels first. */
static void print_packets(const trace_t *t, const frame_identity_frame_t *f, const uint8_t *zero, const uint8_t *before,
                          const uint32_t *drawer, const uint8_t *start, const uint8_t *previous, int cw, int ch, uint64_t shown) {
    uint32_t (*count)[2] = calloc(t->drawer_count + 1, sizeof(*count));
    uint64_t by_cause[CAUSES] = {0}, total = 0;
    size_t k, n = 0;
    unsigned c;
    if (!count) return;
    for (k = 0; k <= t->drawer_count; ++k) count[k][0] = (uint32_t)k;
    for (k = 0; k < (size_t)cw * ch; ++k) {
        unsigned cause;
        if (!memcmp(start + k * 3, previous + k * 3, 3)) continue;
        cause = blamed(zero[k], before[k]);
        ++by_cause[cause]; ++total;
        if (cause == zero[k]) ++count[drawer[k]][1];
    }
    printf("Packets at VBlank %" PRIu64 ": %" PRIu64 " pixels jump;", shown, total);
    for (c = 0; c < CAUSES; ++c) if (by_cause[c]) printf(" %s=%" PRIu64, cause_names[c], by_cause[c]);
    printf("\n");
    qsort(count + 1, t->drawer_count, sizeof(*count), by_pixels);
    for (k = 1; k <= t->drawer_count && n < 80 && count[k][1]; ++k, ++n) {
        const drawer_t *d = &t->drawers[count[k][0] - 1];
        unsigned op = d->words[0] >> 24, i;
        printf("  packet %u op=%02x %s pixels=%u box=%d,%d-%d,%d", count[k][0], op, cause_names[d->cause], count[k][1],
               d->x0, d->y0, d->x1, d->y1);
        if (op >= 0x20 && op < 0x40 && f && d->serial >= f->first_polygon && d->serial - f->first_polygon < f->polygon_count) {
            const frame_identity_polygon_t *q = &f->polygons[d->serial - f->first_polygon];
            printf(" serial=%" PRIu64 " moving=%x corners=", d->serial, d->mask);
            for (i = 0; i < q->corners; ++i) {
                const char *s = q->state[i] < sizeof(states) / sizeof(*states) ? states[q->state[i]] : "?";
                printf("%s%s/%u/%d", i ? "," : "", s, q->age[i], q->projection[i]);
            }
        }
        printf(" words=");
        for (i = 0; i < d->count; ++i) printf("%s%08x", i ? "," : "", d->words[i]);
        printf("\n");
    }
    free(count);
}

int main(int argc, char **argv) {
    const char *path = NULL, *out = NULL;
    uint64_t from = 0, until = UINT64_MAX, sheet = 0;
    int cw = 320, ch = 240, i, failed = 0, have_sheet = 0, verbose = 0;
    frame_record_reader_t in;
    frame_record_frame_t frame;
    frame_blend_t *blend = frame_blend_create();
    counted_t truth, scratch;
    checkpoint_t a, b;
    journal_t j;
    FILE *off_file = NULL, *on_file = NULL;
    uint8_t *now, *before, *drawn, *start, *last;
    uint32_t *jumps, *smooth;
    uint64_t vblank = 0, frame_vblank = 0, previous_end = 0, interval = 0, last_step = 0, last_interval = 0;
    unsigned reason = FRAME_BLEND_NO_HISTORY, reason_before = FRAME_BLEND_NO_HISTORY;
    int have_frame = 0, interval_ready = 0, have_before = 0;
    /* Totals over the window. */
    uint64_t pictures = 0, picture_wrong = 0, frames = 0, frames_checked = 0, frames_wrong = 0, wrong_pixels = 0;
    uint64_t frames_zero_same = 0, changed = 0, jumped = 0, game_frames = 0, renders = 0, stray = 0, stray_frames = 0;
    uint64_t holds = 0, skips = 0, split = 0, reset[FRAME_BLEND_REASONS] = {0}, jumps_blended = 0, jumps_held = 0;
    uint64_t polygons_3d = 0, polygons_moving = 0, corners_3d = 0, corners_moving = 0;
    uint64_t held_service = 0, held_projection = 0, held_neighbour = 0, held_reset = 0, held_jump = 0;
    uint64_t sized = 0, resized = 0, resized_renders = 0, corners_turning = 0, runs_turning = 0, runs_attached = 0;
    uint64_t runs_flat = 0, runs_far = 0;
    char resized_list[1024] = "";
    char wrong_list[1024] = "", stray_list[1024] = "", reset_list[2048] = "", jump_list[1024] = "";
    /* --causes: what drew each pixel of the last two renders, on the canvas. */
    int causes = 0, have_probe = 0;
    uint64_t probe = 0, cause_total[CAUSES] = {0};
    trace_t trace;
    uint8_t *cause_zero = NULL, *cause_one = NULL, *cause_before = NULL;
    uint32_t *drawer_zero = NULL, *cause_count = NULL;
    uint64_t hole_pixels = 0, hole_renders = 0;
    char hole_list[2048] = "";

    for (i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--from") && i + 1 < argc) from = strtoull(argv[++i], NULL, 10);
        else if (!strcmp(argv[i], "--until") && i + 1 < argc) until = strtoull(argv[++i], NULL, 10);
        else if (!strcmp(argv[i], "--out") && i + 1 < argc) out = argv[++i];
        else if (!strcmp(argv[i], "--frames")) verbose = 1;
        else if (!strcmp(argv[i], "--causes")) causes = 1;
        else if (!strcmp(argv[i], "--straight")) frame_blend_straight(blend, 1);
        else if (!strcmp(argv[i], "--packets") && i + 1 < argc) { causes = 1; probe = strtoull(argv[++i], NULL, 10); }
        else if (!strcmp(argv[i], "--sheet") && i + 1 < argc) sheet = strtoull(argv[++i], NULL, 10);
        else if (!strcmp(argv[i], "--canvas") && i + 1 < argc) {
            if (sscanf(argv[++i], "%dx%d", &cw, &ch) != 2 || cw < 16 || ch < 16 || cw > WIDTH || ch > HEIGHT) {
                fprintf(stderr, "Invalid canvas: %s\n", argv[i]); return 2;
            }
        } else if (argv[i][0] != '-' && !path) path = argv[i];
        else {
            fprintf(stderr, "Usage: inbetween RECORD [--from V] [--until V] [--out DIR] [--canvas WxH] [--sheet V] [--frames] [--causes] [--packets V] [--straight]\n");
            return 2;
        }
    }
    if (!path || !blend || !frame_record_open(&in, path)) { fprintf(stderr, "Cannot read recording: %s\n", path ? path : ""); return 2; }
    memset(&frame, 0, sizeof(frame)); memset(&a, 0, sizeof(a)); memset(&b, 0, sizeof(b)); memset(&j, 0, sizeof(j));
    truth.gpu = gpu_create(); scratch.gpu = gpu_create(); truth.serial = scratch.serial = 0;
    now = calloc((size_t)cw * ch, 3); before = calloc((size_t)cw * ch, 3); drawn = calloc((size_t)cw * ch, 3);
    start = calloc((size_t)cw * ch, 3); last = calloc((size_t)cw * ch, 3);
    jumps = calloc((size_t)cw * ch, sizeof(*jumps)); smooth = calloc((size_t)cw * ch, sizeof(*smooth));
    if (!truth.gpu || !scratch.gpu || !now || !before || !drawn || !start || !last || !jumps || !smooth) return 2;
    memset(&trace, 0, sizeof(trace));
    if (causes) {
        size_t n = (size_t)cw * ch;
        trace.shadow = malloc((size_t)WIDTH * HEIGHT * sizeof(*trace.shadow));
        trace.before = malloc((size_t)WIDTH * HEIGHT * sizeof(*trace.before));
        trace.cause = malloc((size_t)WIDTH * HEIGHT);
        trace.drawer = malloc((size_t)WIDTH * HEIGHT * sizeof(*trace.drawer));
        cause_zero = calloc(n, 1); cause_one = calloc(n, 1); cause_before = calloc(n, 1);
        drawer_zero = calloc(n, sizeof(*drawer_zero)); cause_count = calloc(n * CAUSES, sizeof(*cause_count));
        if (!trace.shadow || !trace.before || !trace.cause || !trace.drawer || !cause_zero || !cause_one || !cause_before ||
            !drawer_zero || !cause_count) return 2;
    }
    gpu_observe(truth.gpu, &counter, &truth);
    gpu_observe(scratch.gpu, &counter, &scratch);
    if (out) {
        char file[4096];
        snprintf(file, sizeof(file), "%s/off.rgb", out); off_file = fopen(file, "wb");
        snprintf(file, sizeof(file), "%s/on.rgb", out); on_file = fopen(file, "wb");
        if (!off_file || !on_file) { fprintf(stderr, "Cannot write to %s\n", out); return 2; }
    }

    while (frame_record_next(&in)) {
        const uint8_t *p = in.payload;
        size_t size = in.size;
        if (in.tag == FRAME_RECORD_SNAP) {
            if (size < 24 || !restore(&truth, p + 24, size - 24, le64(p + 8))) { fprintf(stderr, "Damaged GPU state\n"); return 2; }
            frame_blend_reset(blend);
            reason = reason_before = FRAME_BLEND_NO_HISTORY;
            a.valid = b.valid = 0; j.chunks.size = j.mark = 0;
            have_frame = interval_ready = have_before = 0;
        } else if (in.tag == FRAME_RECORD_FRAM) {
            const frame_blend_info_t *info;
            if (!frame_record_parse(&frame, p, size)) { fprintf(stderr, "Damaged identity frame\n"); return 2; }
            if (!frame_blend_add(blend, &frame.frame)) { fprintf(stderr, "Out of memory\n"); return 2; }
            info = frame_blend_info(blend);
            reason_before = reason; reason = info->reason;
            /* The new game frame shows from here; it was drawn since the
             * display start changed before, the checkpoint that becomes a. */
            if (b.valid) {
                checkpoint_t t = a; a = b; b = t;
                memmove(j.chunks.data, j.chunks.data + j.mark, j.chunks.size - j.mark);
                j.chunks.size -= j.mark;
            }
            if (!save(&b, &truth)) { fprintf(stderr, "Cannot keep the GPU state\n"); return 2; }
            j.mark = j.chunks.size;
            /* A game frame that comes before the one it follows reached
             * alpha 1 cuts that one short: the picture skips ahead. */
            if (last_step && last_step < last_interval) ++skips;
            interval = have_frame ? frame.frame.vblank - previous_end : 0;
            previous_end = frame.frame.vblank;
            have_frame = 1;
            frame_vblank = 0;
            /* A frame whose own projections cut or have no history still
             * blends what it draws from the frame before: in battle that is
             * all of its 3D, and a cut shows a game frame later. Each corner
             * is held by the frame its projection belongs to. */
            interval_ready = a.valid && interval > 0 && interval <= FRAME_IDENTITY_GAP;
            last_step = 0; last_interval = interval;
            if (frame.frame.vblank + 1 >= from && frame.frame.vblank < until) {
                ++frames;
                ++reset[info->reason];
                if (info->reason != FRAME_BLEND_KEPT && strlen(reset_list) < sizeof(reset_list) - 40)
                    snprintf(reset_list + strlen(reset_list), sizeof(reset_list) - strlen(reset_list), " %" PRIu64 ":%s",
                             frame.frame.vblank + 1, reasons[info->reason]);
                if (verbose)
                    printf("frame %" PRIu64 " vblank=%" PRIu64 " reason=%s move=%u change=%u screen=%u moving=%u/%u polygons=%u/%u\n",
                           info->number, frame.frame.vblank + 1, reasons[info->reason], info->move, info->change, info->screen_move,
                           info->corners_moving, info->corners_3d, info->polygons_moving, info->polygons_3d);
                if (info->screen_move >= 32) {
                    /* Frame identity's summary counts these as jumps. */
                    if (info->reason == FRAME_BLEND_KEPT) {
                        ++jumps_blended;
                        if (strlen(jump_list) < sizeof(jump_list) - 40)
                            snprintf(jump_list + strlen(jump_list), sizeof(jump_list) - strlen(jump_list), " %" PRIu64 ":%upx",
                                     frame.frame.vblank + 1, info->screen_move);
                    } else ++jumps_held;
                }
                polygons_3d += info->polygons_3d; polygons_moving += info->polygons_moving;
                corners_3d += info->corners_3d; corners_moving += info->corners_moving;
                held_service += info->held_service; held_projection += info->held_projection;
                held_neighbour += info->held_neighbour; held_reset += info->held_reset; held_jump += info->held_jump;
                corners_turning += info->corners_turning; runs_turning += info->runs_turning; runs_attached += info->runs_attached;
                runs_flat += info->runs_flat; runs_far += info->runs_far;
            }
            continue; /* not journalled: it changes no GPU */
        } else if (in.tag == FRAME_RECORD_VBLK) {
            uint64_t recorded = size >= 16 ? le64(p + 8) : 0;
            int in_window, first_of_frame;
            vblank = size >= 8 ? le64(p) : vblank + 1;
            if (vblank > until) break;
            in_window = vblank >= from && vblank <= until;
            /* The picture the VBlank shows is taken before it changes the field. */
            if (frame_record_picture(truth.gpu) != recorded) {
                ++picture_wrong;
                if (strlen(wrong_list) < sizeof(wrong_list) - 40)
                    snprintf(wrong_list + strlen(wrong_list), sizeof(wrong_list) - strlen(wrong_list), " replay@%" PRIu64, vblank);
            }
            ++pictures;
            memcpy(before, now, (size_t)cw * ch * 3);
            picture(truth.gpu, now, cw, ch);
            first_of_frame = have_frame && !frame_vblank;
            if (first_of_frame) frame_vblank = vblank;
            /* Before the window only the digests are checked: drawing every game
             * frame again is what takes the time. */
            if (first_of_frame && a.valid && vblank + FRAME_IDENTITY_GAP >= from) {
                /* Alpha 1 must be the game's picture; alpha 0 against the one before. */
                blend_at_t at = {blend, 1, 1, {0, 0, 0}, 0, NULL, &frame.frame, reason, reason_before, 0};
                size_t k, differ = 0;
                /* cause_before keeps what drew the picture of the game frame before. */
                if (causes) { uint8_t *t = cause_before; cause_before = cause_one; cause_one = t; }
                if (causes) { at.trace = &trace; trace_area(&trace, truth.gpu); trace.describe = 0; }
                if (!restore(&scratch, a.state.data, a.state.size, a.serial)) return 2;
                replay(&scratch, &j, &at);
                if (causes) to_canvas(&trace, cause_one, NULL, cw, ch);
                picture(scratch.gpu, drawn, cw, ch);
                ++frames_checked;
                for (k = 0; k < (size_t)cw * ch * 3; k += 3) if (memcmp(drawn + k, now + k, 3)) ++differ;
                if (differ) {
                    ++frames_wrong; wrong_pixels += differ;
                    if (strlen(wrong_list) < sizeof(wrong_list) - 40)
                        snprintf(wrong_list + strlen(wrong_list), sizeof(wrong_list) - strlen(wrong_list), " alpha1@%" PRIu64 ":%zu", vblank, differ);
                }
                if (interval_ready && have_before) {
                    blend_at_t zero = {blend, 0, 1, {0, 0, 0}, 0, NULL, &frame.frame, reason, reason_before, 0};
                    size_t same = 1;
                    int probing = causes && probe && !have_probe && in_window && frame_vblank >= probe;
                    if (causes) { zero.trace = &trace; trace_area(&trace, truth.gpu); trace.describe = probing; }
                    if (!restore(&scratch, a.state.data, a.state.size, a.serial)) return 2;
                    replay(&scratch, &j, &zero);
                    picture(scratch.gpu, start, cw, ch);
                    if (causes) to_canvas(&trace, cause_zero, drawer_zero, cw, ch);
                    for (k = 0; k < (size_t)cw * ch; ++k) {
                        int change = memcmp(now + k * 3, before + k * 3, 3) != 0;
                        int jump = memcmp(start + k * 3, before + k * 3, 3) != 0;
                        if (jump) same = 0;
                        if (!in_window) continue;
                        changed += change; jumped += jump;
                        if (jump) ++jumps[k]; else if (change) ++smooth[k];
                        if (jump && causes) {
                            unsigned cause = blamed(cause_zero[k], cause_before[k]);
                            ++cause_total[cause]; ++cause_count[k * CAUSES + cause];
                        }
                    }
                    if (probing) {
                        have_probe = 1;
                        print_packets(&trace, &frame.frame, cause_zero, cause_before, drawer_zero, start, before, cw, ch, frame_vblank);
                        if (out) {
                            write_ppm(out, "probe-before.ppm", before, cw, ch);
                            write_ppm(out, "probe-start.ppm", start, cw, ch);
                            write_ppm(out, "probe-game.ppm", now, cw, ch);
                            for (k = 0; k < (size_t)cw * ch; ++k)
                                if (memcmp(start + k * 3, before + k * 3, 3)) {
                                    unsigned cause = blamed(cause_zero[k], cause_before[k]), e;
                                    for (e = 0; e < 3; ++e)
                                        start[k * 3 + e] = (uint8_t)((start[k * 3 + e] + cause_colours[cause][e] * 3) / 4);
                                }
                            write_ppm(out, "probe-causes.ppm", start, cw, ch);
                        }
                    }
                    frames_zero_same += same;
                    if (in_window) ++game_frames;
                    if (verbose && in_window) {
                        size_t c0 = 0, j0 = 0;
                        for (k = 0; k < (size_t)cw * ch; ++k) {
                            c0 += memcmp(now + k * 3, before + k * 3, 3) != 0;
                            j0 += memcmp(start + k * 3, before + k * 3, 3) != 0;
                        }
                        printf("pixels vblank=%" PRIu64 " changed=%zu jumped=%zu\n", vblank, c0, j0);
                    }
                } else if (in_window && have_before) {
                    /* Not blended: everything that changes, changes at once. */
                    for (k = 0; k < (size_t)cw * ch; ++k)
                        if (memcmp(now + k * 3, before + k * 3, 3)) {
                            ++changed; ++jumped; ++jumps[k];
                            if (causes) { ++cause_total[CAUSE_FRAME]; ++cause_count[k * CAUSES + CAUSE_FRAME]; }
                        }
                    ++game_frames;
                }
            }
            if (in_window) {
                const uint8_t *shown = now;
                memcpy(last, now, (size_t)cw * ch * 3);
                if (interval_ready && frame_vblank) {
                    /* One game frame late, on the interval measured between the
                     * last two: alpha reaches 1 when the next is due. */
                    uint64_t step = vblank - frame_vblank + 1;
                    blend_at_t at = {blend, (uint32_t)(step < interval ? step : interval), (uint32_t)interval, {0, 0, 0}, 0, NULL, NULL, reason, reason_before, 0};
                    if (step > interval) ++holds;
                    last_step = step;
                    if (causes) { at.trace = &trace; at.frame = &frame.frame; trace_area(&trace, truth.gpu); trace.describe = 0; }
                    if (!restore(&scratch, a.state.data, a.state.size, a.serial)) return 2;
                    replay(&scratch, &j, &at);
                    picture(scratch.gpu, drawn, cw, ch);
                    shown = drawn;
                    if (causes) {
                        /* A hole: the clear colour, or nothing drawn, where both
                         * game frames had 3D. Geometry the newer frame no longer
                         * draws, or a crack between a held polygon and a moving one. */
                        size_t k, n = 0;
                        to_canvas(&trace, cause_zero, NULL, cw, ch);
                        for (k = 0; k < (size_t)cw * ch; ++k)
                            n += (cause_zero[k] == CAUSE_FILL || cause_zero[k] == CAUSE_NONE) && three_d(cause_before[k]) &&
                                 three_d(cause_one[k]);
                        hole_pixels += n;
                        if (n >= HOLE) {
                            ++hole_renders;
                            if (strlen(hole_list) < sizeof(hole_list) - 40)
                                snprintf(hole_list + strlen(hole_list), sizeof(hole_list) - strlen(hole_list), " %" PRIu64 ":%zu", vblank, n);
                        }
                    }
                    ++renders;
                    stray += at.check.stray; split += at.split;
                    sized += at.check.polygons; resized += at.check.resized;
                    if (at.check.resized) {
                        ++resized_renders;
                        if (strlen(resized_list) < sizeof(resized_list) - 40)
                            snprintf(resized_list + strlen(resized_list), sizeof(resized_list) - strlen(resized_list), " %" PRIu64 ":%u", vblank, at.check.resized);
                    }
                    if (at.check.stray) {
                        ++stray_frames;
                        if (strlen(stray_list) < sizeof(stray_list) - 40)
                            snprintf(stray_list + strlen(stray_list), sizeof(stray_list) - strlen(stray_list), " %" PRIu64 ":%u", vblank, at.check.stray);
                    }
                    if (sheet && !have_sheet && out && frame_vblank >= sheet) {
                        char name[64];
                        if (step == 1) {
                            write_ppm(out, "sheet-before.ppm", before, cw, ch);
                            write_ppm(out, "sheet-start.ppm", start, cw, ch);
                        }
                        snprintf(name, sizeof(name), "sheet-on-%u.ppm", (unsigned)step);
                        write_ppm(out, name, drawn, cw, ch);
                        if (step == interval) { write_ppm(out, "sheet-game.ppm", now, cw, ch); have_sheet = 1; }
                    }
                }
                if (off_file && (fwrite(now, 1, (size_t)cw * ch * 3, off_file) != (size_t)cw * ch * 3 ||
                                 fwrite(shown, 1, (size_t)cw * ch * 3, on_file) != (size_t)cw * ch * 3)) {
                    fprintf(stderr, "Cannot write frames\n"); return 2;
                }
            }
            have_before = 1;
            apply(&truth, in.tag, p, size, NULL);
            if (!journal(&j, in.tag, p, size)) return 2;
            continue;
        } else if (in.tag == FRAME_RECORD_END) break;
        apply(&truth, in.tag, p, size, NULL);
        if (in.tag != FRAME_RECORD_SNAP && !journal(&j, in.tag, p, size)) { fprintf(stderr, "Out of memory\n"); return 2; }
    }
    if (out) {
        uint8_t *heat = malloc((size_t)cw * ch * 3);
        size_t k;
        if (!heat) return 2;
        for (k = 0; k < (size_t)cw * ch; ++k) {
            /* Dimmed grey where nothing changed; where something did, more
             * colour the more often, red for the share of its changes that
             * came at game frames and green for the share that moved between. */
            double grey = (last[k * 3] * 3.0 + last[k * 3 + 1] * 6.0 + last[k * 3 + 2]) / 30.0 * 0.6;
            double total = (double)jumps[k] + smooth[k], a = game_frames ? 4.0 * total / (double)game_frames : 0, r;
            if (a > 1) a = 1;
            r = total > 0 ? jumps[k] / total : 0;
            heat[k * 3] = (uint8_t)(grey * (1 - a) + a * 255 * r + 0.5);
            heat[k * 3 + 1] = (uint8_t)(grey * (1 - a) + a * 255 * (1 - r) + 0.5);
            heat[k * 3 + 2] = (uint8_t)(grey * (1 - a) + 0.5);
        }
        if (!write_ppm(out, "heat.ppm", heat, cw, ch)) failed = 1;
        if (causes) {
            /* Dimmed grey; where pixels jumped at game frames, the colour of
             * what most often caused it, the stronger the more often. */
            for (k = 0; k < (size_t)cw * ch; ++k) {
                double grey = (last[k * 3] * 3.0 + last[k * 3 + 1] * 6.0 + last[k * 3 + 2]) / 30.0 * 0.4, a;
                unsigned c, best = 0, e;
                for (c = 1; c < CAUSES; ++c) if (cause_count[k * CAUSES + c] > cause_count[k * CAUSES + best]) best = c;
                a = game_frames ? 4.0 * jumps[k] / (double)game_frames : 0;
                if (a > 1) a = 1;
                if (!jumps[k]) a = 0;
                for (e = 0; e < 3; ++e) heat[k * 3 + e] = (uint8_t)(grey * (1 - a) + a * cause_colours[best][e] + 0.5);
            }
            if (!write_ppm(out, "causes.ppm", heat, cw, ch)) failed = 1;
        }
        free(heat);
        if (fclose(off_file) || fclose(on_file)) failed = 1;
    }
    failed |= picture_wrong || frames_wrong;
    printf("Pictures: replayed=%" PRIu64 " differ=%" PRIu64 "\n", pictures, picture_wrong);
    printf("Alpha 1: game-frames=%" PRIu64 " differ=%" PRIu64 " pixels=%" PRIu64 "\n", frames_checked, frames_wrong, wrong_pixels);
    printf("Alpha 0: game-frames-equal-to-previous=%" PRIu64 "\n", frames_zero_same);
    printf("Window: vblanks=%" PRIu64 "-%" PRIu64 " game-frames=%" PRIu64 " renders=%" PRIu64 " holds=%" PRIu64 " skips=%" PRIu64 "\n",
           from, until == UINT64_MAX ? vblank : until, frames, renders, holds, skips);
    printf("Blended: corners3d=%" PRIu64 " moving=%" PRIu64 " (%.2f%%) polygons3d=%" PRIu64 " moving=%" PRIu64 " (%.2f%%)"
           " held-service=%" PRIu64 " held-projection=%" PRIu64 " held-neighbour=%" PRIu64 " held-reset=%" PRIu64
           " held-jump=%" PRIu64 "\n",
           corners_3d, corners_moving, corners_3d ? 100.0 * corners_moving / corners_3d : 0.0, polygons_3d, polygons_moving,
           polygons_3d ? 100.0 * polygons_moving / polygons_3d : 0.0, held_service, held_projection, held_neighbour, held_reset,
           held_jump);
    printf("Turning: corners=%" PRIu64 " (%.2f%% of moving) runs=%" PRIu64 " hanging=%" PRIu64 " flat-or-mirrored=%" PRIu64
           " too-far=%" PRIu64 "\n", corners_turning, corners_moving ? 100.0 * corners_turning / corners_moving : 0.0, runs_turning,
           runs_attached, runs_flat, runs_far);
    printf("Game rate: changed=%" PRIu64 " jumped=%" PRIu64 " (%.2f%% of changed, %.2f%% of the screen per game frame)\n",
           changed, jumped, changed ? 100.0 * jumped / changed : 0.0,
           game_frames ? 100.0 * jumped / ((double)game_frames * cw * ch) : 0.0);
    if (causes) {
        uint64_t total = 0;
        for (i = 0; i < CAUSES; ++i) total += cause_total[i];
        printf("Causes (share of the pixels that change only at game frames):");
        for (i = 0; i < CAUSES; ++i)
            if (cause_total[i]) printf(" %s=%.2f%%", cause_names[i], total ? 100.0 * cause_total[i] / total : 0.0);
        printf("\n");
    }
    printf("Resets:");
    for (i = 1; i < FRAME_BLEND_REASONS; ++i) printf(" %s=%" PRIu64, reasons[i], reset[i]);
    printf("\nReset at:%s\n", reset_list);
    printf("Jumps (median paired screen move of 32 pixels or more): held=%" PRIu64 " blended=%" PRIu64 " at:%s\n",
           jumps_held, jumps_blended, jump_list);
    printf("Stray: corners=%" PRIu64 " renders=%" PRIu64 " at:%s\n", stray, stray_frames, stray_list);
    printf("Size (polygons moving whole, with an edge in view space a tenth shorter or longer than at both ends): polygons=%" PRIu64
           " resized=%" PRIu64 " (%.2f%%) renders=%" PRIu64 " at:%s\n", sized, resized, sized ? 100.0 * resized / sized : 0.0,
           resized_renders, resized_list);
    if (causes)
        printf("Holes (the clear colour where both game frames have 3D): pixels=%" PRIu64 " renders=%" PRIu64 " at:%s\n",
               hole_pixels, hole_renders, hole_list);
    printf("Split packets: %" PRIu64 "\n", split);
    printf("Wrong:%s\n", wrong_list);
    frame_record_close(&in);
    frame_record_frame_free(&frame);
    frame_blend_destroy(blend);
    return failed ? 1 : 0;
}
