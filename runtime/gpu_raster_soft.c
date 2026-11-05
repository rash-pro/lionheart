#include "gpu_raster.h"
#include <stdlib.h>
#include <string.h>

static int min(int a, int b) { return a < b ? a : b; }
static int max(int a, int b) { return a > b ? a : b; }
static int clamp(int n, int limit) { return n < 0 ? 0 : n > limit ? limit : n; }
static int floor_div(int n, int divisor) { return n / divisor - (n % divisor < 0); }

void gpu_raster_store(gpu_raster_t *s, int x, int y, uint16_t pixel) {
    uint16_t *dest = &s->vram[((unsigned)y & 511) * 1024 + ((unsigned)x & 1023)];
    if (!(s->mask & 2) || !(*dest & 0x8000))
        *dest = pixel | ((s->mask & 1) ? 0x8000 : 0);
}

void gpu_raster_store_span(gpu_raster_t *s, unsigned x, unsigned y, const uint8_t *data, unsigned pixels) {
    unsigned i;
    if (s->mask) {
        for (i = 0; i < pixels; ++i)
            gpu_raster_store(s, x + i, y, data[i * 2] | (uint16_t)data[i * 2 + 1] << 8);
        return;
    }
    y &= 511;
    while (pixels) {
        unsigned n;
        x &= 1023; n = 1024 - x; if (n > pixels) n = pixels;
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
        memcpy(s->vram + y * 1024 + x, data, n * 2);
#else
        for (i = 0; i < n; ++i) s->vram[y * 1024 + x + i] = data[i*2] | (uint16_t)data[i*2+1] << 8;
#endif
        data += n * 2; x += n; pixels -= n;
    }
}

static uint16_t texture(const gpu_raster_t *s, int u, int v, unsigned clut) {
    unsigned mx = (s->window & 31) * 8, my = ((s->window >> 5) & 31) * 8;
    unsigned tx = ((unsigned)u & 255 & ~mx) | (((s->window >> 10) & 31) * 8 & mx);
    unsigned ty = ((unsigned)v & 255 & ~my) | (((s->window >> 15) & 31) * 8 & my);
    unsigned depth = (s->mode >> 7) & 3;
    unsigned x = (s->mode & 15) * 64, y = ((s->mode >> 4) & 1) * 256;
    unsigned packed, index;
    y = (y + ty) & 511;
    if (depth >= 2) return s->vram[y * 1024 + ((x + tx) & 1023)];
    packed = s->vram[y * 1024 + ((x + (tx >> (2 - depth))) & 1023)];
    index = depth ? (packed >> ((tx & 1) * 8)) & 255 : (packed >> ((tx & 3) * 4)) & 15;
    return s->vram[((clut >> 6) & 511) * 1024 + (((clut & 63) * 16 + index) & 1023)];
}

static void pixel(gpu_raster_t *s, int x, int y, int r, int g, int b,
                  int u, int v, gpu_paint_t p) {
    static const int dither[4][4] = {{-4,0,-3,1},{2,-2,3,-1},{-3,1,-4,0},{3,-1,2,-2}};
    uint16_t texel = 0, out;
    int semi = p.semi;
    int d = p.dither && (s->mode & 512) ? dither[y & 3][x & 3] : 0;
    if (!(s->mode & 1024) && s->display_enabled) {
        if (s->interlaced) {
            if ((y & 1) == s->field) return;
        } else if (((x - s->display_x) & 1023) < s->display_w &&
                   ((y - s->display_y) & 511) < s->display_h) return;
    }
    if (p.textured) {
        texel = texture(s, u, v, p.clut);
        if (!texel) return;
        if (p.raw) {
            r = (texel & 31) * 8; g = ((texel >> 5) & 31) * 8; b = ((texel >> 10) & 31) * 8;
            d = 0;
        } else {
            r = ((texel & 31) * r) / 16;
            g = (((texel >> 5) & 31) * g) / 16;
            b = (((texel >> 10) & 31) * b) / 16;
        }
        semi &= !!(texel & 0x8000);
    }
    r = clamp(r + d, 255) >> 3; g = clamp(g + d, 255) >> 3; b = clamp(b + d, 255) >> 3;
    if (semi) {
        unsigned old = s->vram[y * 1024 + x];
        int br = old & 31, bg = (old >> 5) & 31, bb = (old >> 10) & 31;
        switch ((s->mode >> 5) & 3) {
        case 0: r = (br + r) >> 1; g = (bg + g) >> 1; b = (bb + b) >> 1; break;
        case 1: r += br; g += bg; b += bb; break;
        case 2: r = br - r; g = bg - g; b = bb - b; break;
        case 3: r = br + (r >> 2); g = bg + (g >> 2); b = bb + (b >> 2); break;
        }
        r = clamp(r, 31); g = clamp(g, 31); b = clamp(b, 31);
    }
    out = (uint16_t)(r | (g << 5) | (b << 10) | (texel & 0x8000));
    gpu_raster_store(s, x, y, out);
}

static int edge(gpu_vertex_t a, gpu_vertex_t b, int x, int y) {
    return (b.x - a.x) * (y - a.y) - (b.y - a.y) * (x - a.x);
}

static int top_left(gpu_vertex_t a, gpu_vertex_t b) {
    return b.y < a.y || (b.y == a.y && b.x > a.x);
}

/* The reciprocal rounds down. For a 32-bit numerator its quotient is at
 * most one short; the remainder comparison restores exact integer division. */
static int attribute(int64_t n, unsigned area, uint64_t reciprocal) {
    if ((uint64_t)n <= UINT32_MAX) {
        unsigned q = (unsigned)(((uint64_t)n * reciprocal) >> 32);
        return (int)(q + ((uint64_t)n - (uint64_t)q * area >= area));
    }
    return (int)(n / area);
}

void gpu_raster_triangle(gpu_raster_t *s, gpu_vertex_t a, gpu_vertex_t b,
                         gpu_vertex_t c, gpu_paint_t p) {
    int l = min(a.x, min(b.x, c.x)), r = max(a.x, max(b.x, c.x));
    int t = min(a.y, min(b.y, c.y)), bot = max(a.y, max(b.y, c.y));
    int area, x, y, ea, eb, ec, ta, tb, tc;
    uint64_t reciprocal;
    if (r - l >= 1024 || bot - t >= 512) return;
    area = edge(a, b, c.x, c.y);
    if (!area) return;
    if (area < 0) { gpu_vertex_t tmp = b; b = c; c = tmp; area = -area; }
    reciprocal = (UINT64_C(1) << 32) / (unsigned)area;
    l = max(l, s->left); r = min(r, s->right);
    t = max(t, s->top); bot = min(bot, s->bottom);
    ta = top_left(b, c); tb = top_left(c, a); tc = top_left(a, b);
    /* PS1 samples integer screen positions, not pixel centers at (x+.5,y+.5). */
    for (y = t; y <= bot; ++y) {
        ea = edge(b, c, l, y); eb = edge(c, a, l, y); ec = edge(a, b, l, y);
        for (x = l; x <= r; ++x) {
            if ((ea > 0 || (!ea && ta)) && (eb > 0 || (!eb && tb)) && (ec > 0 || (!ec && tc))) {
#define ATTR(name) (a.name == b.name && a.name == c.name ? a.name : \
    attribute((int64_t)ea * a.name + (int64_t)eb * b.name + (int64_t)ec * c.name, (unsigned)area, reciprocal))
                int red = a.r, green = a.g, blue = a.b, u = 0, v = 0;
                if (!p.textured || !p.raw) { red = ATTR(r); green = ATTR(g); blue = ATTR(b); }
                if (p.textured) { u = ATTR(u); v = ATTR(v); }
                pixel(s, x, y, red, green, blue, u, v, p);
#undef ATTR
            }
            ea -= c.y - b.y; eb -= a.y - c.y; ec -= b.y - a.y;
        }
    }
}

void gpu_raster_rectangle(gpu_raster_t *s, gpu_vertex_t a, int w, int h, gpu_paint_t p) {
    int l = max(a.x, s->left), r = min(a.x + w - 1, s->right);
    int t = max(a.y, s->top), b = min(a.y + h - 1, s->bottom), x, y;
    int du = (s->mode & 0x1000) ? -1 : 1, dv = (s->mode & 0x2000) ? -1 : 1;
    for (y = t; y <= b; ++y) for (x = l; x <= r; ++x)
        pixel(s, x, y, a.r, a.g, a.b, a.u + (x - a.x) * du, a.v + (y - a.y) * dv, p);
}

void gpu_raster_line(gpu_raster_t *s, gpu_vertex_t a, gpu_vertex_t b, gpu_paint_t p, int skip_first) {
    int dx = abs(b.x - a.x), dy = abs(b.y - a.y), n = max(dx, dy), i;
    if (dx >= 1024 || dy >= 512) return;
    /* Fixed-point DDA: include endpoints, but do not blend a polyline joint twice. */
    for (i = skip_first; i <= n; ++i) {
        int x = a.x, y = a.y, r = a.r, g = a.g, blue = a.b;
        if (n) {
            x = floor_div(a.x * n + (b.x - a.x) * i + n / 2, n);
            y = floor_div(a.y * n + (b.y - a.y) * i + n / 2, n);
            r += (b.r - a.r) * i / n; g += (b.g - a.g) * i / n; blue += (b.b - a.b) * i / n;
        }
        if (x >= s->left && x <= s->right && y >= s->top && y <= s->bottom)
            pixel(s, x, y, r, g, blue, 0, 0, p);
    }
}

void gpu_raster_fill(gpu_raster_t *s, int x, int y, int w, int h, uint32_t color) {
    int i, j;
    uint16_t value = (uint16_t)(((color >> 3) & 31) | ((color >> 6) & 0x3e0) | ((color >> 9) & 0x7c00));
    for (j = 0; j < h; ++j) for (i = 0; i < w; ++i)
        s->vram[((y + j) & 511) * 1024 + ((x + i) & 1023)] = value;
}

void gpu_raster_copy(gpu_raster_t *s, int sx, int sy, int dx, int dy, int w, int h) {
    int y, x;
    /* Preserve horizontal overlap; rows advance top-to-bottom, allowing vertical feedback. */
    uint16_t row[1024];
    for (y = 0; y < h; ++y) {
        for (x = 0; x < w; ++x) row[x] = s->vram[((sy + y) & 511) * 1024 + ((sx + x) & 1023)];
        for (x = 0; x < w; ++x) gpu_raster_store(s, dx + x, dy + y, row[x]);
    }
}
