/* The scalar barycentric oracle shares only the unchanged pixel rules. */
#include "../runtime/gpu_raster_soft.c"
#include <assert.h>

static void reference_triangle(gpu_raster_t *s, gpu_vertex_t a, gpu_vertex_t b,
                               gpu_vertex_t c, gpu_paint_t p) {
    int l = min(a.x, min(b.x, c.x)), r = max(a.x, max(b.x, c.x));
    int t = min(a.y, min(b.y, c.y)), bot = max(a.y, max(b.y, c.y));
    int area = edge(a, b, c.x, c.y);
    if (r - l >= 1024 || bot - t >= 512 || !area) return;
    if (area < 0) { gpu_vertex_t tmp = b; b = c; c = tmp; area = -area; }
    for (int y = max(t, s->top); y <= min(bot, s->bottom); ++y)
        for (int x = max(l, s->left); x <= min(r, s->right); ++x) {
            int ea = edge(b, c, x, y), eb = edge(c, a, x, y), ec = edge(a, b, x, y);
            if (ea < 0 || (!ea && !top_left(b, c)) || eb < 0 || (!eb && !top_left(c, a)) ||
                ec < 0 || (!ec && !top_left(a, b))) continue;
#define REF(name) ((int)(((int64_t)ea*a.name + (int64_t)eb*b.name + (int64_t)ec*c.name) / area))
            pixel(s, x, y, REF(r), REF(g), REF(b), REF(u), REF(v), p);
#undef REF
        }
}

static uint32_t seed = 1;
static unsigned random_word(void) { seed = seed * 1664525u + 1013904223u; return seed; }

int main(void) {
    gpu_raster_t *a = calloc(1, sizeof(*a)), *b = malloc(sizeof(*b));
    assert(a && b);
    /* Areas at both hardware limits, powers of two, and their neighbors;
     * numerators just below/on/above exact quotient boundaries. */
    for (unsigned area = 1; area <= 1023u * 511u; ++area) {
        uint64_t reciprocal = (UINT64_C(1) << 32) / area;
        for (unsigned value = 0; value <= 255; value += 17) {
            int64_t n = (int64_t)area * value;
            for (int delta = -1; delta <= 1; ++delta)
                assert(attribute(n + delta, area, reciprocal) == (n + delta) / area);
        }
        assert(attribute(UINT32_MAX, area, reciprocal) == (int)(UINT32_MAX / area));
        assert(attribute(INT64_C(1) << 40, area, reciprocal) == (int)((INT64_C(1) << 40) / area));
    }
    for (unsigned i = 0; i < 1024 * 512; ++i) a->vram[i] = (uint16_t)random_word();
    for (unsigned trial = 0; trial < 1600; ++trial) {
        gpu_vertex_t v[3];
        for (unsigned j = 0; j < 3; ++j) {
            v[j] = (gpu_vertex_t){(int)(random_word() >> 24) - 32,
                (int)(random_word() >> 24) - 32, (int)(random_word() >> 24),
                (int)(random_word() >> 24), (int)(random_word() >> 24),
                (int)(random_word() >> 24), (int)(random_word() >> 24)};
        }
        if (trial % 3 == 0) for (unsigned j = 1; j < 3; ++j)
            v[j].r=v[0].r, v[j].g=v[0].g, v[j].b=v[0].b;
        if (trial % 7 == 0) v[1].u=v[2].u=v[0].u;
        if (trial % 11 == 0) v[1].v=v[2].v=v[0].v;
        if (trial % 13 == 0) v[0].u=-100, v[2].v=512;
        a->mode=random_word() & 2047; a->window=random_word() & 0xfffff;
        a->mask=trial % 4; a->left=trial % 17; a->right=190; a->top=trial % 19; a->bottom=191;
        a->display_enabled=trial % 2; a->display_x=64; a->display_y=64;
        a->display_w=40; a->display_h=40; a->interlaced=trial % 3 == 0; a->field=trial % 2;
        gpu_paint_t p={!!(trial & 1),!!(trial & 2),!!(trial & 4),!!(trial & 8),random_word() & 0x7fff};
        memcpy(b,a,sizeof(*a));
        gpu_raster_triangle(a,v[0],v[1],v[2],p);
        reference_triangle(b,v[0],v[1],v[2],p);
        assert(!memcmp(a,b,sizeof(*a)));
    }
    free(a); free(b); return 0;
}
