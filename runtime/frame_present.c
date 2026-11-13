#include "frame_present.h"
#include "frame_blend.h"
#include "frame_layer.h"
#include "gpu_raster.h"
#include <stdlib.h>
#include <string.h>

/* The clock. */
void frame_present_clock_reset(frame_present_clock_t *c) { memset(c, 0, sizeof(*c)); }

void frame_present_clock_frame(frame_present_clock_t *c, uint64_t vblank, int blendable) {
    uint64_t interval = c->frames ? vblank - c->end : 0;
    /* The frame before was cut short when it had not reached 1. */
    if (c->ready && c->reached < 1) ++c->skips;
    c->ready = blendable && c->frames && interval > 0 && interval <= FRAME_IDENTITY_GAP;
    c->interval = interval;
    c->end = vblank;
    c->reached = 0;
    ++c->frames;
}

int frame_present_clock_phase(frame_present_clock_t *c, double t, uint32_t *num, uint32_t *den) {
    /* Fractions in 1/256 of a VBlank: finer than any display shows, and the
     * same steps as a presenter that draws once per VBlank when t is whole. */
    double step = t - (double)c->end;
    *num = *den = 1;
    if (!c->ready || t < 0) return 0;
    if (step < 0) step = 0;
    *den = (uint32_t)c->interval * 256u;
    *num = step * 256.0 + 0.5 >= (double)*den ? *den : (uint32_t)(step * 256.0 + 0.5);
    if (*num == *den) {
        /* Drawn past the end of its interval: the next frame is late. */
        if (step > (double)c->interval) ++c->holds;
        c->reached = 1;
        return 0;
    }
    if ((double)*num / *den > c->reached) c->reached = (double)*num / *den;
    ++c->between;
    return 1;
}

/* The presenter. */
enum { CHUNK_GP0, CHUNK_GP1, CHUNK_READ, CHUNK_VBLANK };

typedef struct {
    uint8_t *data;
    size_t size, capacity;
} bytes_t;

struct frame_present {
    gpu_t *gpu, *checkpoint;
    uint64_t serial, checkpoint_serial; /* the presentation GPU's next polygon */
    frame_present_renderer_t renderer;
    int fractions, precision, precise_applied;
    /* The guest's PGXP corners of the polygons since the checkpoint, in
     * serial order. */
    struct corners { uint64_t serial; unsigned mask; float at[4][3]; } *corners;
    size_t corner_count, corner_capacity;
    /* Chunks since the checkpoint: a tag byte, the length in words, the
     * words. mark is where the newest game frame ended. */
    bytes_t journal;
    size_t mark;
    int have_mark;
    /* Where the journal stood at the guest's last VBlank: the picture the
     * game shows when nothing is blended. */
    size_t vblank;
    /* The newest frame was drawn whole, and the checkpoint moved to its end:
     * the journal holds only what came after it. */
    int consumed;
    frame_blend_t *blend;
    frame_layer_t *layer;
    frame_present_clock_t clock;
    uint64_t vblanks;
    int failed;
    frame_present_totals_t totals;
};

static void count_polygon(void *context, unsigned opcode, const uint32_t *packet) {
    (void)opcode; (void)packet;
    ++((frame_present_t *)context)->serial;
}
static void ignore_start(void *context, uint32_t start) { (void)context; (void)start; }
static void ignore_vblank(void *context) { (void)context; }
static void complete(void *context, const uint32_t *packet, unsigned count);
static const gpu_observer_t counter = {ignore_start, ignore_vblank, count_polygon, complete};

frame_present_t *frame_present_create(void) {
    frame_present_t *p = calloc(1, sizeof(*p));
    if (!p) return NULL;
    p->gpu = gpu_create(); p->checkpoint = gpu_create();
    p->blend = frame_blend_create(); p->layer = frame_layer_create();
    if (!p->gpu || !p->checkpoint || !p->blend || !p->layer) { frame_present_destroy(p); return NULL; }
    gpu_observe(p->gpu, &counter, p);
    frame_present_clock_reset(&p->clock);
    return p;
}

void frame_present_destroy(frame_present_t *p) {
    if (!p) return;
    free(p->gpu); free(p->checkpoint);
    frame_blend_destroy(p->blend); frame_layer_destroy(p->layer);
    free(p->journal.data); free(p->corners);
    free(p);
}

gpu_t *frame_present_gpu(frame_present_t *p) { return p->gpu; }
void frame_present_renderer(frame_present_t *p, const frame_present_renderer_t *r) {
    if (r) p->renderer = *r; else memset(&p->renderer, 0, sizeof(p->renderer));
}
void frame_present_fractions(frame_present_t *p, int on) { p->fractions = on; }
void frame_present_precision(frame_present_t *p, int mode) { p->precision = mode; }
uint64_t frame_present_vblanks(const frame_present_t *p) { return p->vblanks; }
const frame_present_clock_t *frame_present_timing(const frame_present_t *p) { return &p->clock; }
frame_present_totals_t frame_present_totals(const frame_present_t *p) { return p->totals; }

static void save(frame_present_t *p) {
    gpu_copy(p->checkpoint, p->gpu);
    p->checkpoint_serial = p->serial;
    {
        /* What is drawn from here on starts at the checkpoint's serial. */
        size_t k = 0;
        while (k < p->corner_count && p->corners[k].serial < p->serial) ++k;
        if (k < p->corner_count)
            memmove(p->corners, p->corners + k, (p->corner_count - k) * sizeof(*p->corners));
        p->corner_count -= k;
    }
    if (p->renderer.save) p->renderer.save(p->renderer.context);
}

static void restore(frame_present_t *p) {
    gpu_copy(p->gpu, p->checkpoint);
    p->serial = p->checkpoint_serial;
    gpu_subpixel_offset(p->gpu, 0, 0);
    if (p->renderer.restore) p->renderer.restore(p->renderer.context);
}

void frame_present_sync(frame_present_t *p, gpu_t *guest, uint64_t polygon) {
    gpu_copy(p->gpu, guest);
    p->serial = polygon;
    gpu_subpixel_offset(p->gpu, 0, 0);
    save(p);
    p->journal.size = 0; p->mark = 0; p->have_mark = 0; p->consumed = 0; p->vblank = 0;
    p->corner_count = 0;
    frame_blend_reset(p->blend); frame_layer_reset(p->layer);
    frame_present_clock_reset(&p->clock);
    p->vblanks = 0; p->failed = 0;
    ++p->totals.syncs;
}

static void append(frame_present_t *p, unsigned tag, const uint8_t *words, unsigned count) {
    bytes_t *j = &p->journal;
    size_t need = 5 + (size_t)count * 4;
    if (p->failed) return;
    if (j->size + need > j->capacity) {
        size_t c = j->capacity ? j->capacity * 2 : 1 << 16;
        uint8_t *d;
        while (c < j->size + need) c *= 2;
        if (!(d = realloc(j->data, c))) { p->failed = 1; ++p->totals.failures; return; }
        j->data = d; j->capacity = c;
    }
    j->data[j->size] = (uint8_t)tag;
    memcpy(j->data + j->size + 1, &count, 4);
    if (count) memcpy(j->data + j->size + 5, words, (size_t)count * 4);
    j->size += need;
}

void frame_present_gp0(frame_present_t *p, const uint8_t *words, unsigned count) { append(p, CHUNK_GP0, words, count); }
void frame_present_gp1(frame_present_t *p, uint32_t word) {
    uint8_t w[4] = {(uint8_t)word, (uint8_t)(word >> 8), (uint8_t)(word >> 16), (uint8_t)(word >> 24)};
    append(p, CHUNK_GP1, w, 1);
}
void frame_present_read(frame_present_t *p) { append(p, CHUNK_READ, NULL, 0); }
void frame_present_vblank(frame_present_t *p) {
    append(p, CHUNK_VBLANK, NULL, 0);
    ++p->vblanks;
    if (!p->failed) p->vblank = p->journal.size;
}

void frame_present_command(frame_present_t *p, const uint32_t *packet, unsigned count, uint64_t polygon) {
    unsigned op = packet[0] >> 24;
    if (p->failed) return;
    if (op >= 0x20 && op < 0x40) frame_blend_packet(p->blend, polygon, packet);
    frame_blend_command(p->blend, packet, count);
    if (!frame_layer_command(p->layer, packet, count)) { p->failed = 1; ++p->totals.failures; }
}

void frame_present_corners(frame_present_t *p, uint64_t polygon, unsigned mask, float corner[4][3]) {
    if (p->failed || !mask) return;
    if (p->corner_count == p->corner_capacity) {
        size_t n = p->corner_capacity ? p->corner_capacity * 2 : 4096;
        struct corners *c = realloc(p->corners, n * sizeof(*c));
        if (!c) { p->failed = 1; ++p->totals.failures; return; }
        p->corners = c; p->corner_capacity = n;
    }
    p->corners[p->corner_count].serial = polygon;
    p->corners[p->corner_count].mask = mask;
    memcpy(p->corners[p->corner_count].at, corner, sizeof(p->corners[0].at));
    ++p->corner_count;
}

/* Drawing again. */
typedef struct {
    uint32_t num, den;
    int after;               /* past the display-start change that ends the frame */
    int beneath;             /* what the frame culled at the screen's edge is drawn */
    uint32_t offsets, sprites;
    int blended;             /* something was drawn elsewhere than recorded */
} at_t;

static uint32_t le32(const uint8_t *b) { return (uint32_t)b[0] | (uint32_t)b[1] << 8 | (uint32_t)b[2] << 16 | (uint32_t)b[3] << 24; }
static void put_le32(uint8_t *b, uint32_t v) { b[0] = (uint8_t)v; b[1] = (uint8_t)(v >> 8); b[2] = (uint8_t)(v >> 16); b[3] = (uint8_t)(v >> 24); }
static int16_t low(uint32_t w) { return (int16_t)(uint16_t)(w & 0xffffu); }
static int16_t high(uint32_t w) { return (int16_t)(uint16_t)(w >> 16); }

/* What the frame before drew at the screen's edge and the frame blended
 * culled, beneath its first polygon. No polygon of the stream: the serial
 * stays that of the next one. */
static void beneath(frame_present_t *p, at_t *at) {
    uint64_t serial = p->serial;
    unsigned i, n, k;
    uint32_t page = gpu_raster_state(p->gpu)->mode;
    int drawn = 0;
    at->beneath = 1;
    if (at->num && at->num < at->den) {
        const gpu_raster_t *state = gpu_raster_state(p->gpu);
        uint32_t mode = 0xe1000000u | state->mode;
        uint32_t window = 0xe2000000u | state->window;
        for (i = 0; i < frame_blend_info(p->blend)->polygon_count; ++i) {
            uint32_t ground[14];
            uint8_t packet[56];
            n = frame_blend_ground(p->blend, i, at->num, at->den, ground);
            if (!n) continue;
            gpu_gp0(p->gpu, ground[0]);
            gpu_gp0(p->gpu, ground[1]);
            for (k = 2; k < n; ++k) put_le32(packet + (k - 2) * 4, ground[k]);
            /* Ground under the picture stays at whole pixels: it is no polygon of
             * the stream, and must not take the next one's precision. */
            if (p->precision && p->fractions) p->precise_applied = 1;
            gpu_gp0_words(p->gpu, packet, n - 2);
            /* Injected ground must not consume recorded polygon identities. */
            p->serial = serial;
        }
        gpu_gp0(p->gpu, mode);
        gpu_gp0(p->gpu, window);
    }

    for (i = 0; i < frame_blend_leftovers(p->blend); ++i) {
        uint32_t words[12];
        uint8_t packet[12 * 4];
        if (!(n = frame_blend_leftover(p->blend, i, at->num, at->den, words))) continue;
        if (p->precision && p->fractions) {
            float xy[4][2], depth[4] = {0, 0, 0, 0}, fraction[4][2] = {{0, 0}, {0, 0}, {0, 0}, {0, 0}};
            unsigned op = words[0] >> 24, corners = (op & 8) ? 4 : 3, c, word = 1, known = 0;
            frame_blend_leftover_precise(p->blend, i, at->num, at->den, xy, depth);
            for (c = 0; c < corners; ++c) {
                if (c && (op & 16)) ++word;
                fraction[c][0] = xy[c][0] - 0.5f - low(words[word]);
                fraction[c][1] = xy[c][1] - 0.5f - high(words[word]);
                word += (op & 4) ? 2 : 1;
                if (depth[c] > 0) ++known;
            }
            gpu_subpixel(p->gpu, fraction);
            if (p->precision == 2 && known == corners) gpu_subpixel_depth(p->gpu, depth);
            p->precise_applied = 1;
            p->totals.corners_blended += corners;
        }
        for (k = 0; k < n; ++k) put_le32(packet + k * 4, words[k]);
        gpu_gp0_words(p->gpu, packet, n);
        p->serial = serial;
        at->blended = 1;
        drawn = 1;
    }
    /* A textured polygon sets the texture page, its semi-transparency
     * included, for what is drawn after it: the frame's first polygon must
     * find the game's (an untextured shadow would otherwise blend in the
     * page's mode of something culled at the edge). */
    if (drawn) {
        uint8_t e1[4];
        put_le32(e1, 0xe1000000u | (page & 0x3fffu));
        gpu_gp0_words(p->gpu, e1, 1);
    }
}

/* The guest's PGXP corners of a polygon, by serial. */
static const struct corners *corners_of(const frame_present_t *p, uint64_t serial) {
    size_t lo = 0, hi = p->corner_count;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (p->corners[mid].serial < serial) lo = mid + 1;
        else hi = mid;
    }
    return lo < p->corner_count && p->corners[lo].serial == serial ? &p->corners[lo] : NULL;
}

/* With precision, the corners of the polygon packet about to be drawn to a
 * fraction of a pixel, at num / den: those blending places where the GTE put
 * them before it dropped the fraction, game frames included, so that both
 * ends of every interval are as fine as the motion between them; those it
 * does not where the guest's PGXP put them for this packet. As PGXP draws
 * them, each stands half a pixel up and left of that point, the PlayStation's
 * pixel at its centre, and a polygon whose every corner has a depth maps its
 * texture in perspective. A corner that follows the polygons around it has
 * no projection of its own and moves by fractions as without precision.
 * moved is the mask of corners blending moved; packet holds the corners'
 * whole pixels as drawn. */
static void precise(frame_present_t *p, const uint8_t *packet, unsigned op, uint32_t num, uint32_t den, unsigned moved) {
    float fraction[4][2] = {{0, 0}, {0, 0}, {0, 0}, {0, 0}}, depth[4] = {0, 0, 0, 0}, exact[4][2], deep[4], whole[4][2];
    const struct corners *guest = corners_of(p, p->serial);
    unsigned corners = (op & 8) ? 4 : 3, k = 1, i, known = 0;
    unsigned placed = frame_blend_precise(p->blend, p->serial, num, den, exact, deep), following = moved & ~placed;
    if (following) following &= frame_blend_fractions(p->blend, p->serial, num, den, whole);
    for (i = 0; i < corners; ++i) {
        uint32_t xy;
        if (i && (op & 16)) ++k;
        xy = le32(packet + k * 4);
        k += (op & 4) ? 2 : 1;
        if (placed & (1u << i)) {
            fraction[i][0] = exact[i][0] - 0.5f - low(xy); fraction[i][1] = exact[i][1] - 0.5f - high(xy);
            depth[i] = deep[i];
            ++p->totals.corners_blended;
        } else if (following & (1u << i)) {
            fraction[i][0] = whole[i][0] - low(xy); fraction[i][1] = whole[i][1] - high(xy);
            ++p->totals.corners_whole;
        } else if (!(moved & (1u << i)) && guest && (guest->mask & (1u << i))) {
            fraction[i][0] = guest->at[i][0] - low(xy); fraction[i][1] = guest->at[i][1] - high(xy);
            depth[i] = guest->at[i][2];
            ++p->totals.corners_pgxp;
        } else ++p->totals.corners_whole;
        if (depth[i] > 0) ++known;
    }
    gpu_subpixel(p->gpu, fraction);
    if (p->precision == 2 && known == corners) gpu_subpixel_depth(p->gpu, depth);
    p->precise_applied = 1;
}

static void complete(void *context, const uint32_t *words, unsigned count) {
    frame_present_t *p = context;
    unsigned op = words[0] >> 24, i;
    if (op < 0x20 || op >= 0x40 || !p->precision || !p->fractions) return;
    /* A packet split across chunks cannot blend, but once complete it has
     * the same endpoint precision as an unsplit packet and its checkpoint. */
    if (!p->precise_applied) {
        uint8_t packet[12 * 4];
        for (i = 0; i < count; ++i) put_le32(packet + i * 4, words[i]);
        precise(p, packet, op, 1, 1, 0);
    }
    p->precise_applied = 0;
}

/* GP0 words, with the newest frame's polygons, offsets and sprites where
 * blending and its layers put them at the fraction, as tools/devtools/inbetween.c
 * draws them offline. */
static void feed(frame_present_t *p, const uint8_t *w, unsigned words, at_t *at) {
    gpu_t *g = p->gpu;
    while (words) {
        unsigned n = gpu_data_words(g), op, i;
        uint8_t packet[12 * 4];
        if (n) {
            if (n > words) n = words;
            gpu_gp0_words(g, w, n);
            w += n * 4; words -= n;
            continue;
        }
        op = w[3];
        n = gpu_packet_words(op);
        if (n > words || !at || at->num >= at->den) {
            /* At the end of the interval everything is as recorded; a
             * packet cut between chunks is drawn as recorded too. */
            if (n > words) n = words;
            else if (op >= 0x20 && op < 0x40 && p->precision && p->fractions) precise(p, w, op, 1, 1, 0);
            gpu_gp0_words(g, w, n);
            w += n * 4; words -= n;
            continue;
        }
        memcpy(packet, w, n * 4);
        if (op >= 0x20 && op < 0x40 && !at->beneath && p->serial >= frame_blend_info(p->blend)->first_polygon)
            beneath(p, at);
        if (!at->after && op == 0xe5) {
            uint32_t word;
            float fx = 0, fy = 0;
            if (frame_layer_offset(p->layer, at->offsets, at->num, at->den, &word)) { put_le32(packet, word); at->blended = 1; }
            if (p->fractions) frame_layer_offset_fraction(p->layer, at->offsets, at->num, at->den, &fx, &fy);
            gpu_subpixel_offset(g, fx, fy);
            ++at->offsets;
        } else if (!at->after && op >= 0x60 && op < 0x80) {
            uint32_t word = le32(packet + 4);
            float fraction[4][2] = {{0, 0}, {0, 0}, {0, 0}, {0, 0}};
            if (frame_layer_sprite(p->layer, at->sprites, at->num, at->den, &word)) at->blended = 1;
            put_le32(packet + 4, word);
            if (p->fractions && frame_layer_sprite_fraction(p->layer, at->sprites, at->num, at->den, &fraction[0][0], &fraction[0][1]))
                gpu_subpixel(g, fraction);
            ++at->sprites;
        } else if (op >= 0x20 && op < 0x40) {
            uint32_t xy[4], colours[4];
            unsigned corners = (op & 8) ? 4 : 3, k = 1, mask, shades;
            int visible = frame_blend_visible(p->blend, p->serial, at->num, at->den);
            mask = frame_blend_polygon(p->blend, p->serial, at->num, at->den, xy, NULL);
            if (!visible) {
                /* Consume the packet and its state, without rasterizing it. */
                xy[0] = le32(packet + 4);
                for (i = 1; i < corners; ++i) xy[i] = xy[0];
                mask = (1u << corners) - 1;
            }
            shades = frame_blend_colours(p->blend, p->serial, at->num, at->den, colours);
            for (i = 0; i < corners; ++i) {
                if (i && (op & 16)) {
                    if (shades & (1u << i)) put_le32(packet + k * 4, (le32(packet + k * 4) & 0xff000000u) | colours[i]);
                    ++k;
                }
                if (mask & (1u << i)) put_le32(packet + k * 4, xy[i]);
                ++k;
                if (op & 4) ++k;
            }
            if (shades & 1u) put_le32(packet, (le32(packet) & 0xff000000u) | colours[0]);
            if (mask || shades) at->blended = 1;
            /* A polygon collapsed to a point gets no part of a pixel: finer than
             * a pixel, that would give it area back. */
            if (!visible) p->precise_applied = p->precision && p->fractions;
            else if (p->precision && p->fractions) precise(p, packet, op, at->num, at->den, mask);
            else if (mask && p->fractions) {
                float exact[4][2], fraction[4][2] = {{0, 0}, {0, 0}, {0, 0}, {0, 0}};
                unsigned moving = frame_blend_fractions(p->blend, p->serial, at->num, at->den, exact);
                for (i = 0; i < corners; ++i)
                    if (moving & mask & (1u << i)) {
                        fraction[i][0] = exact[i][0] - low(xy[i]);
                        fraction[i][1] = exact[i][1] - high(xy[i]);
                    }
                gpu_subpixel(g, fraction);
            }
        }
        gpu_gp0_words(g, packet, n);
        w += n * 4; words -= n;
    }
}

/* Whether a rectangle of VRAM meets the picture the presentation GPU shows. */
static int shown(const gpu_raster_t *s, int x0, int y0, int x1, int y1) {
    int w = s->display_w, h = s->display_h, dx, dy;
    if (!s->display_enabled || w <= 0 || h <= 0) return 0;
    for (dy = -512; dy <= 0; dy += 512)
        for (dx = -1024; dx <= 0; dx += 1024) {
            int l = s->display_x + dx, t = s->display_y + dy;
            if (x0 < l + w && l <= x1 && y0 < t + h && t <= y1) return 1;
        }
    return 0;
}

/* After the newest frame's display-start change the guest draws the next
 * frame into the other buffer; none of that reaches the picture, and none of
 * it is kept (the checkpoint moves only at a display-start change), so
 * drawing commands whose drawing area misses the shown picture are left out.
 * Everything else (uploads, copies, state) is drawn, and what is drawn into
 * the shown picture (the HUD) as it was. */
static void feed_after(frame_present_t *p, const uint8_t *w, unsigned words) {
    gpu_t *g = p->gpu;
    const gpu_raster_t *s = gpu_raster_state(g);
    while (words) {
        unsigned n = gpu_data_words(g), op;
        int skip = 0;
        if (!n) {
            op = w[3];
            n = gpu_packet_words(op);
            if (n <= words) {
                if (((op >= 0x20 && op < 0x40) || (op >= 0x40 && op < 0x60 && !(op & 8)) || (op >= 0x60 && op < 0x80)) &&
                    !shown(s, s->left, s->top, s->right, s->bottom)) skip = 1;
                else if (op == 0x02) {
                    uint32_t a = le32(w + 4), b = le32(w + 8);
                    int x = (int)(a & 0x3f0), y = (int)((a >> 16) & 511);
                    int fw = (int)(((b & 1023) + 15) & ~15u), fh = (int)((b >> 16) & 511);
                    skip = fw && fh && !shown(s, x, y, x + fw - 1, y + fh - 1) && x + fw <= 1024 && y + fh <= 512;
                }
                if (!skip && op >= 0x20 && op < 0x40 && p->precision && p->fractions) precise(p, w, op, 1, 1, 0);
            }
        }
        if (n > words) n = words;
        if (skip && op >= 0x20 && op < 0x40) ++p->serial;
        if (!skip) gpu_gp0_words(g, w, n);
        w += n * 4; words -= n;
    }
}

static void apply(frame_present_t *p, unsigned tag, const uint8_t *w, unsigned count, at_t *at, int after) {
    if (tag == CHUNK_GP0 && after) feed_after(p, w, count);
    else if (tag == CHUNK_GP0) feed(p, w, count, at);
    else if (tag == CHUNK_GP1) gpu_gp1(p->gpu, le32(w));
    else if (tag == CHUNK_READ) (void)gpu_read(p->gpu);
    else gpu_vblank(p->gpu);
    p->totals.replayed_words += count;
}

/* Drops the chunks before a place in the journal: the checkpoint is now there. */
static void trim(frame_present_t *p, size_t to) {
    memmove(p->journal.data, p->journal.data + to, p->journal.size - to);
    p->journal.size -= to;
    p->mark = p->mark > to ? p->mark - to : 0;
    p->vblank = p->vblank > to ? p->vblank - to : 0;
}

/* Draws the journal from the checkpoint. With at, the chunks before the mark
 * are the newest frame, drawn at its fraction. When that fraction is 1 the
 * state at the mark is exact, and becomes the checkpoint. until bounds the
 * chunks drawn. */
static void replay(frame_present_t *p, at_t *at, size_t until, int all) {
    size_t i = 0;
    int whole = !at || at->num >= at->den;
    if (p->renderer.between) p->renderer.between(p->renderer.context, !whole && !p->consumed);
    restore(p);
    while (i + 5 <= until) {
        unsigned tag = p->journal.data[i], count;
        if (p->have_mark && !p->consumed && i == p->mark && whole) {
            save(p);
            p->consumed = 1;
        }
        memcpy(&count, p->journal.data + i + 1, 4);
        if (at) at->after = p->have_mark && !p->consumed && i >= p->mark;
        apply(p, tag, p->journal.data + i + 5, count, p->consumed || (at && at->after) ? NULL : at,
              !all && p->have_mark && (p->consumed || i >= p->mark));
        i += 5 + (size_t)count * 4;
    }
    if (p->have_mark && !p->consumed && i == p->mark && whole && until >= p->mark) {
        save(p);
        p->consumed = 1;
    }
    if (p->consumed && p->mark) trim(p, p->mark);
}

int frame_present_frame(frame_present_t *p, const frame_identity_frame_t *f, int32_t width, int32_t height) {
    const frame_blend_info_t *info;
    int history;
    if (p->failed) return 0;
    /* The frame before ends where this one starts: the checkpoint goes there,
     * drawing it whole once more if no draw got it to 1. */
    if (p->have_mark && !p->consumed) { replay(p, NULL, p->mark, 1); ++p->totals.advances; }
    p->mark = p->journal.size; p->have_mark = 1; p->consumed = 0;
    if (width > 0 && height > 0) frame_blend_screen(p->blend, width, height);
    if (!frame_blend_add(p->blend, f)) { p->failed = 1; ++p->totals.failures; return 0; }
    info = frame_blend_info(p->blend);
    /* The layers move only where the 3D blends from the frame before. */
    history = info->reason == FRAME_BLEND_KEPT;
    if (!frame_layer_end(p->layer, history)) { p->failed = 1; ++p->totals.failures; return 0; }
    /* A frame whose own projections cut or have no history still blends
     * what it draws from the frame before (battle draws each frame from the
     * projections of the one before); excluded scenes are shown whole. */
    frame_present_clock_frame(&p->clock, p->vblanks, !(f->flags & FRAME_IDENTITY_EXCLUDED));
    return 1;
}

int frame_present_draw(frame_present_t *p, double t) {
    at_t at;
    int settle;
    ++p->totals.draws;
    memset(&at, 0, sizeof(at));
    at.num = at.den = 1;
    if (!p->failed && p->have_mark) frame_present_clock_phase(&p->clock, t, &at.num, &at.den);
    if (p->consumed) at.num = at.den = 1;
    /* Long without a frame (a load, a movie that sets no display start), or
     * none yet since the presenter started over (the screens before the
     * first): nothing will blend from here, so the checkpoint follows the
     * guest and the journal stays short; then everything is drawn, as it is
     * kept, up to the guest's last VBlank. A guest that ends no frames draws
     * into the picture it shows, and what it has drawn of the next one by
     * the time the display asks is not a picture the game shows. */
    settle = p->have_mark ? p->consumed && p->vblanks - p->clock.end > FRAME_IDENTITY_GAP : 1;
    replay(p, p->failed ? NULL : &at, settle && !p->failed ? p->vblank : p->journal.size, settle);
    if (settle) {
        /* After a failure nothing more is kept: what there is goes. */
        size_t to = p->failed ? p->journal.size : p->vblank;
        save(p);
        trim(p, to);
        ++p->totals.settles;
    }
    if (at.blended && at.num < at.den) { ++p->totals.blended; return 1; }
    return 0;
}
