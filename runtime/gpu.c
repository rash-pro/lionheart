#include "gpu.h"
#include "gpu_raster.h"
#include "frame_oracle.h"
#if defined(FF8_PGXP)
#include "pgxp.h"
#endif
#include <stdlib.h>

struct gpu {
    gpu_raster_t raster;
    uint32_t packet[12], read_latch, draw_start, draw_end, draw_offset;
    unsigned count, needed, opcode;
    int offset_x, offset_y;
    unsigned transfer; /* 0: commands, 1: CPU upload, 2: CPU readback */
    unsigned tx, ty, tw, th, cursor;
    int polyline, poly_color_pending;
    uint32_t poly_color;
    gpu_vertex_t poly_last;
    gpu_paint_t poly_paint;
    unsigned display_start, horizontal, vertical, display_mode, disabled, dma, irq, field;
    /* Host-side only: never saved, and a GPU restored from a state has none. */
    const gpu_observer_t *observer;
    void *observer_context;
};

static int signed11(unsigned n) { return (int)(n & 1023) - (int)(n & 1024); }
static unsigned width(uint32_t n) { return ((n - 1) & 1023) + 1; }
static unsigned height(uint32_t n) { return (((n >> 16) - 1) & 511) + 1; }

static void display_state(gpu_t *g) {
    gpu_raster_t *s = &g->raster;
    int rgb24;
    gpu_display(g, &s->display_x, &s->display_y, &s->display_w, &s->display_h,
                &rgb24, &s->display_enabled);
    if (rgb24) s->display_w = (s->display_w * 3 + 1) / 2;
    s->interlaced = (g->display_mode & 36) == 36;
    s->field = (int)g->field;
}

static gpu_vertex_t vertex(gpu_t *g, uint32_t xy, uint32_t color, uint32_t uv) {
    gpu_vertex_t v;
    v.x = signed11(xy) + g->offset_x; v.y = signed11(xy >> 16) + g->offset_y;
    v.r = color & 255; v.g = (color >> 8) & 255; v.b = (color >> 16) & 255;
    v.u = uv & 255; v.v = (uv >> 8) & 255;
    return v;
}

static gpu_paint_t paint(unsigned op) {
    gpu_paint_t p;
    p.textured = !!(op & 4); p.raw = !!(op & 1); p.semi = !!(op & 2);
    p.dither = 0; p.clut = 0;
    return p;
}

static void reset_fifo(gpu_t *g) {
    g->count = g->needed = g->transfer = g->cursor = 0;
    g->polyline = g->poly_color_pending = 0;
}

static void reset(gpu_t *g) {
#if defined(FF8_PGXP)
    pgxp_reset(g);
#endif
    reset_fifo(g);
    g->raster.mode = g->raster.window = g->raster.mask = 0;
    g->raster.left = g->raster.top = g->raster.right = g->raster.bottom = 0;
    g->draw_start = g->draw_end = g->draw_offset = 0;
    g->offset_x = g->offset_y = 0;
    g->display_start = g->display_mode = g->dma = g->irq = g->field = g->read_latch = 0;
    g->horizontal = 0x200 | ((0x200 + 256 * 10) << 12);
    g->vertical = 0x10 | ((0x10 + 240) << 10);
    g->disabled = 1;
    display_state(g);
}

void gpu_observe(gpu_t *g, const gpu_observer_t *observer, void *context) {
    g->observer = observer; g->observer_context = context;
}

gpu_t *gpu_create(void) {
    gpu_t *g = calloc(1, sizeof(*g));
    if (g) reset(g);
    return g;
}

static unsigned packet_size(unsigned op) {
    if (op >= 0x20 && op < 0x40) {
        unsigned n = (op & 8) ? 4 : 3;
        return 1 + n * (1 + !!(op & 4)) + ((op & 16) ? n - 1 : 0);
    }
    if (op >= 0x40 && op < 0x60) return (op & 16) ? 4 : 3;
    if (op >= 0x60 && op < 0x80) return 2 + !!(op & 4) + !(op & 24);
    if (op >= 0x80 && op < 0xa0) return 4;
    if (op >= 0xa0 && op < 0xe0) return 3;
    return op == 2 ? 3 : 1;
}

static void execute(gpu_t *g) {
    uint32_t *w = g->packet;
    unsigned op = g->opcode, group = op >> 5;
    gpu_raster_t *s = &g->raster;
    gpu_paint_t p = paint(op);
    if (group == 1) {
        gpu_vertex_t v[4];
        unsigned i, at = 1, n = (op & 8) ? 4 : 3;
        uint32_t color = w[0];
        for (i = 0; i < n; ++i) {
            uint32_t xy, uv = 0;
            if (i && (op & 16)) color = w[at++];
            xy = w[at++];
            if (p.textured) {
                uv = w[at++];
                if (!i) p.clut = uv >> 16;
                if (i == 1) s->mode = (s->mode & ~0x9ffu) | ((uv >> 16) & 0x9ff);
            }
            v[i] = vertex(g, xy, color, uv);
        }
        p.dither = !!(op & 16) || (p.textured && !p.raw);
        FRAME_ORACLE(frame_oracle_polygon(op));
        if (g->observer) g->observer->polygon(g->observer_context, op, w);
#if defined(FF8_PGXP)
        pgxp_polygon(g, v, n, g->offset_x, g->offset_y);
#endif
        gpu_raster_triangle(s, v[0], v[1], v[2], p);
        if (n == 4) {
#if defined(FF8_PGXP)
            pgxp_select(g, 1);
#endif
            gpu_raster_triangle(s, v[1], v[2], v[3], p);
        }
    } else if (group == 2) {
        gpu_vertex_t a = vertex(g, w[1], w[0], 0);
        gpu_vertex_t b = vertex(g, w[(op & 16) ? 3 : 2], (op & 16) ? w[2] : w[0], 0);
        p.textured = p.raw = 0; p.dither = 1;
        gpu_raster_line(s, a, b, p, 0);
        if (op & 8) {
            g->polyline = 1; g->poly_last = b; g->poly_color = w[0]; g->poly_paint = p;
        }
    } else if (group == 3) {
        unsigned size = (op >> 3) & 3;
        int rw, rh;
        uint32_t uv = p.textured ? w[2] : 0;
        gpu_vertex_t v = vertex(g, w[1], w[0], uv);
        p.clut = uv >> 16;
        if (!size) { uint32_t wh = w[p.textured ? 3 : 2]; rw = wh & 1023; rh = (wh >> 16) & 511; }
        else { rw = rh = size == 1 ? 1 : size == 2 ? 8 : 16; }
        gpu_raster_rectangle(s, v, rw, rh, p);
    } else if (group == 4) {
        gpu_raster_copy(s, w[1] & 1023, (w[1] >> 16) & 511,
                        w[2] & 1023, (w[2] >> 16) & 511, (int)width(w[3]), (int)height(w[3]));
    } else if (group == 5 || group == 6) {
        g->tx = w[1] & 1023; g->ty = (w[1] >> 16) & 511;
        g->tw = width(w[2]); g->th = height(w[2]); g->cursor = 0;
        g->transfer = group == 5 ? 1 : 2;
    } else switch (op) {
    case 0x02:
        gpu_raster_fill(s, w[1] & 0x3f0, (w[1] >> 16) & 511,
                        ((w[2] & 1023) + 15) & ~15u, (w[2] >> 16) & 511, w[0]); break;
    case 0x1f: g->irq = 1; break;
    case 0xe1: s->mode = w[0] & 0x3fff; break;
    case 0xe2: s->window = w[0] & 0xfffff; break;
    case 0xe3:
        g->draw_start = w[0] & 0x7ffff;
        s->left = w[0] & 1023; s->top = (w[0] >> 10) & 511; break;
    case 0xe4:
        g->draw_end = w[0] & 0x7ffff;
        s->right = w[0] & 1023; s->bottom = (w[0] >> 10) & 511; break;
    case 0xe5:
        g->draw_offset = w[0] & 0x3fffff;
        g->offset_x = signed11(w[0]); g->offset_y = signed11(w[0] >> 11); break;
    case 0xe6: s->mask = w[0] & 3; break;
    default: break; /* NOP and cache clear: this backend has no texture/CLUT cache. */
    }
}

static inline void upload_word(gpu_t *g, uint32_t word) {
        unsigned i;
        for (i = 0; i < 2 && g->cursor < g->tw * g->th; ++i, ++g->cursor)
            gpu_raster_store(&g->raster, g->tx + g->cursor % g->tw,
                             g->ty + g->cursor / g->tw, (uint16_t)(word >> (i * 16)));
        if (g->cursor == g->tw * g->th) g->transfer = 0;
}

void gpu_gp0(gpu_t *g, uint32_t word) {
    if (g->transfer == 1) { upload_word(g, word); return; }
    if (g->polyline) {
        int gouraud = !!(g->opcode & 16);
        if (!g->poly_color_pending && (word & 0xf000f000) == 0x50005000) {
            g->polyline = 0; return;
        }
        if (gouraud && !g->poly_color_pending) { g->poly_color = word; g->poly_color_pending = 1; }
        else {
            gpu_vertex_t next = vertex(g, word, g->poly_color, 0);
            gpu_raster_line(&g->raster, g->poly_last, next, g->poly_paint, 1);
            g->poly_last = next; g->poly_color_pending = 0;
        }
        return;
    }
    if (!g->count) { g->opcode = word >> 24; g->needed = packet_size(g->opcode); }
    FRAME_ORACLE(frame_oracle_packet_word(g->count, NULL));
    g->packet[g->count++] = word;
    if (g->count == g->needed) { execute(g); g->count = 0; }
}

static uint32_t le_word(const uint8_t *p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

void gpu_gp0_words(gpu_t *g, const uint8_t *data, unsigned words) {
    while (words) {
        unsigned n, i;
        if (g->transfer == 1) {
            unsigned pixels = g->tw * g->th - g->cursor, consumed;
            const uint8_t *source = data;
            if (pixels > words * 2) pixels = words * 2;
            consumed = (pixels + 1) / 2;
            while (pixels) {
                unsigned row = g->cursor / g->tw, column = g->cursor % g->tw;
                n = g->tw - column; if (n > pixels) n = pixels;
                gpu_raster_store_span(&g->raster, g->tx + column, g->ty + row, source, n);
                source += n * 2; g->cursor += n; pixels -= n;
            }
            if (g->cursor == g->tw * g->th) g->transfer = 0;
            data += consumed * 4; words -= consumed; continue;
        }
        if (g->polyline) {
            gpu_gp0(g, le_word(data)); data += 4; --words; continue;
        }
        if (!g->count) { g->opcode = data[3]; g->needed = packet_size(g->opcode); }
        n = g->needed - g->count;
        if (n > words) n = words;
        for (i = 0; i < n; ++i) {
            FRAME_ORACLE(frame_oracle_packet_word(g->count, data + i * 4));
            g->packet[g->count++] = le_word(data + i * 4);
        }
        data += n * 4; words -= n;
        if (g->count == g->needed) { execute(g); g->count = 0; }
    }
}

void gpu_gp1(gpu_t *g, uint32_t word) {
    unsigned op = (word >> 24) & 63;
    switch (op) {
    case 0: reset(g); break;
    case 1: reset_fifo(g); break;
    case 2: g->irq = 0; break;
    case 3: g->disabled = word & 1; break;
    case 4: g->dma = word & 3; break;
    case 5:
        g->display_start = word & 0x7ffff;
        if (g->observer) g->observer->display_start(g->observer_context, g->display_start);
        break;
    case 6: g->horizontal = word & 0xffffff; break;
    case 7: g->vertical = word & 0xfffff; break;
    case 8: g->display_mode = word & 0x7f; break;
    default:
        if (op >= 0x10 && op <= 0x1f) switch (word & 15) {
        case 2: g->read_latch = g->raster.window; break;
        case 3: g->read_latch = g->draw_start; break;
        case 4: g->read_latch = g->draw_end; break;
        case 5: g->read_latch = g->draw_offset; break;
        case 7: g->read_latch = 2; break;
        case 8: g->read_latch = 0; break;
        default: break;
        }
        break;
    }
    display_state(g);
}

uint32_t gpu_read(gpu_t *g) {
    if (g->transfer == 2) {
        unsigned i;
        uint32_t result = 0;
        for (i = 0; i < 2; ++i, ++g->cursor) {
            unsigned x = (g->tx + g->cursor % g->tw) & 1023;
            unsigned y = (g->ty + g->cursor / g->tw) & 511;
            result |= (uint32_t)g->raster.vram[y * 1024 + x] << (i * 16);
        }
        if (g->cursor >= g->tw * g->th) g->transfer = 0;
        g->read_latch = result;
    }
    return g->read_latch;
}

uint32_t gpu_status(gpu_t *g) {
    unsigned mode = g->display_mode;
    unsigned idle = !g->count && !g->polyline && !g->transfer;
    unsigned read_ready = g->transfer == 2;
    unsigned write_ready = !read_ready && !g->polyline && !(g->count && g->opcode >= 0x20 && g->opcode < 0x60);
    unsigned request = g->dma == 1 ? !read_ready : g->dma == 2 ? write_ready : g->dma == 3 ? read_ready : 0;
    uint32_t result = (g->raster.mode & 0x7ff) | (g->raster.mask << 11)
        | (((mode & 32) ? g->field : 1) << 13) | ((g->raster.mode & 0x800) << 4)
        | ((mode & 64) << 10) | ((mode & 63) << 17) | (g->disabled << 23)
        | (g->irq << 24) | (request << 25) | (idle << 26) | (read_ready << 27)
        | (write_ready << 28) | (g->dma << 29);
    if (mode & 32) result |= (uint32_t)g->field << 31;
    return result;
}

uint32_t gpu_status_scanline(gpu_t *g, unsigned line) {
    uint32_t value = gpu_status(g);
    /* Retain the existing 480i field phase. In 240-line modes GPUSTAT.31
     * follows visible scanline parity, including across an interlace change. */
    if ((g->display_mode & 36) == 36) return value;
    value &= 0x7fffffffu;
    if (line >= (g->vertical & 1023) && line < (g->vertical >> 10))
        value |= (line & 1u) << 31;
    return value;
}

void gpu_vblank(gpu_t *g) {
    g->field ^= 1; g->raster.field = (int)g->field;
    if (g->observer) g->observer->vblank(g->observer_context);
#if defined(FF8_PGXP)
    pgxp_vblank(g);
#endif
}
gpu_raster_t *gpu_raster_state(gpu_t *g) { return &g->raster; }
const uint16_t *gpu_vram(gpu_t *g) { return g->raster.vram; }

gpu_scanout_t gpu_scanout(const gpu_t *g) {
    gpu_scanout_t out = {g->horizontal, g->vertical, g->display_mode};
    return out;
}

void gpu_display(gpu_t *g, int *x, int *y, int *w, int *h, int *rgb24, int *enabled) {
    static const int divisors[4] = {10, 8, 5, 4};
    int divisor = (g->display_mode & 64) ? 7 : divisors[g->display_mode & 3];
    int horizontal = (int)(g->horizontal >> 12) - (int)(g->horizontal & 4095);
    int vertical = (int)(g->vertical >> 10) - (int)(g->vertical & 1023);
    *x = g->display_start & 1023; *y = (g->display_start >> 10) & 511;
    *w = horizontal > 0 ? ((horizontal / divisor + 2) & ~3) : 0;
    *h = vertical > 0 ? vertical * ((g->display_mode & 36) == 36 ? 2 : 1) : 0;
    if (*w > 1024) *w = 1024;
    if (*h > 512) *h = 512;
    *rgb24 = !!(g->display_mode & 16); *enabled = !g->disabled;
}

#if defined(FF8_SAVESTATES)
#define FF8_SS_PRIVATE
#include "savestate_gpu.c"
#endif
