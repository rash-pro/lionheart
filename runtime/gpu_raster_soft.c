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
    if (s->fragment && !((s->mask & 2) && (s->vram[y * 1024 + x] & 0x8000)))
        s->fragment(s->fragment_context, x, y, semi);
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
#if defined(__GNUC__) || defined(__clang__)
__attribute__((unused))
#endif
static int attribute(int64_t n, unsigned area, uint64_t reciprocal) {
    if ((uint64_t)n <= UINT32_MAX) {
        unsigned q = (unsigned)(((uint64_t)n * reciprocal) >> 32);
        return (int)(q + ((uint64_t)n - (uint64_t)q * area >= area));
    }
    return (int)(n / area);
}

static const int dither_table[4][4] = {{-4,0,-3,1},{2,-2,3,-1},{-3,1,-4,0},{3,-1,2,-2}};

/* Reciprocal lookup table for |edge_dy| in 1..511 (guaranteed since bot-t < 512),
 * eliminating every hardware division from scanline X clipping. */
static uint64_t edge_recip[512];
static int edge_recip_ready;

static void init_edge_recip(void) {
    unsigned i;
    for (i = 1; i < 512; ++i) edge_recip[i] = (UINT64_C(1) << 32) / i;
    edge_recip_ready = 1;
}

static inline unsigned div_u32_recip(unsigned n, unsigned d, uint64_t r) {
    unsigned q = (unsigned)(((uint64_t)n * r) >> 32);
    return q + ((uint64_t)n - (uint64_t)q * d >= d);
}

static inline void divmod_u64_recip(uint64_t n, unsigned area, uint64_t reciprocal, int *q_out, int *r_out) {
    if (n <= UINT32_MAX) {
        unsigned q = (unsigned)((n * reciprocal) >> 32);
        uint64_t rem = n - (uint64_t)q * area;
        if (rem >= area) { rem -= area; ++q; }
        *q_out = (int)q;
        *r_out = (int)rem;
    } else {
        *q_out = (int)(n / area);
        *r_out = (int)(n % area);
    }
}

typedef struct {
    unsigned tex_umask, tex_uoff, tex_vmask, tex_voff;
    unsigned tex_depth, tex_page_x, tex_page_y;
    const uint16_t *tbase;
    const uint16_t *clut_row;
    const uint16_t *clut_direct;
    unsigned clut_x;
    unsigned blend_mode;
    uint16_t mask_set;
    int use_dither, mask_check, display_clip, has_fragment, fast_tex;
} raster_env_t;

static inline void raster_env_init(raster_env_t *e, const gpu_raster_t *s, gpu_paint_t p) {
    unsigned mx = (s->window & 31u) * 8u, my = ((s->window >> 5) & 31u) * 8u;
    e->tex_umask = 255u & ~mx;
    e->tex_uoff = (((s->window >> 10) & 31u) * 8u) & mx;
    e->tex_vmask = 255u & ~my;
    e->tex_voff = (((s->window >> 15) & 31u) * 8u) & my;
    e->tex_depth = (s->mode >> 7) & 3u;
    e->tex_page_x = (s->mode & 15u) * 64u;
    e->tex_page_y = ((s->mode >> 4) & 1u) * 256u;
    e->tbase = &s->vram[e->tex_page_y * 1024u + e->tex_page_x];
    e->clut_row = &s->vram[((p.clut >> 6) & 511u) * 1024u];
    e->clut_x = (p.clut & 63u) * 16u;
    e->clut_direct = e->clut_row + e->clut_x;
    e->blend_mode = (s->mode >> 5) & 3u;
    e->mask_set = (s->mask & 1u) ? 0x8000u : 0u;
    e->use_dither = p.dither && (s->mode & 512u);
    e->mask_check = (s->mask & 2u) != 0;
    e->display_clip = !(s->mode & 1024u) && s->display_enabled;
    e->has_fragment = s->fragment != NULL;
    /* fast_tex: window disabled and texel/CLUT indexing cannot wrap past 1023. */
    if ((s->window & 0x3ffu) == 0u) {
        if (e->tex_depth == 0u) e->fast_tex = 1;
        else if (e->tex_depth == 1u) e->fast_tex = (e->tex_page_x <= 896u && e->clut_x <= 768u) ? 2 : 0;
        else e->fast_tex = (e->tex_page_x <= 768u) ? 3 : 0;
    } else {
        e->fast_tex = 0;
    }
}

static inline uint16_t fetch_texel_fast(const gpu_raster_t *s, const raster_env_t *e, int u, int v, int mode) {
    if (mode == 1) {
        unsigned tx = (unsigned)u & 255u, ty = (unsigned)v & 255u;
        unsigned packed = e->tbase[ty * 1024u + (tx >> 2)];
        return e->clut_direct[(packed >> ((tx & 3u) * 4u)) & 15u];
    }
    if (mode == 2) {
        unsigned tx = (unsigned)u & 255u, ty = (unsigned)v & 255u;
        unsigned packed = e->tbase[ty * 1024u + (tx >> 1)];
        return e->clut_direct[(packed >> ((tx & 1u) * 8u)) & 255u];
    }
    if (mode == 3) {
        unsigned tx = (unsigned)u & 255u, ty = (unsigned)v & 255u;
        return e->tbase[ty * 1024u + tx];
    }
    {
        unsigned tx = ((unsigned)u & e->tex_umask) | e->tex_uoff;
        unsigned ty = ((unsigned)v & e->tex_vmask) | e->tex_voff;
        const uint16_t *trow = &s->vram[(e->tex_page_y + ty) * 1024u];
        if (e->tex_depth >= 2u) return trow[(e->tex_page_x + tx) & 1023u];
        if (e->tex_depth) {
            unsigned packed = trow[(e->tex_page_x + (tx >> 1)) & 1023u];
            unsigned idx = (packed >> ((tx & 1u) * 8u)) & 255u;
            return e->clut_row[(e->clut_x + idx) & 1023u];
        } else {
            unsigned packed = trow[(e->tex_page_x + (tx >> 2)) & 1023u];
            unsigned idx = (packed >> ((tx & 3u) * 4u)) & 15u;
            return e->clut_row[(e->clut_x + idx) & 1023u];
        }
    }
}

static inline uint16_t blend_pixel(uint16_t old, int r, int g, int b, unsigned mode, uint16_t hi) {
    int br = (int)(old & 31u), bg = (int)((old >> 5) & 31u), bb = (int)((old >> 10) & 31u);
    switch (mode) {
    case 0: r = (br + r) >> 1; g = (bg + g) >> 1; b = (bb + b) >> 1; break;
    case 1: r += br; g += bg; b += bb; break;
    case 2: r = br - r; g = bg - g; b = bb - b; break;
    default: r = br + (r >> 2); g = bg + (g >> 2); b = bb + (b >> 2); break;
    }
    r = clamp(r, 31); g = clamp(g, 31); b = clamp(b, 31);
    return (uint16_t)((unsigned)r | ((unsigned)g << 5) | ((unsigned)b << 10) | hi);
}

static inline void emit_fast_pixel(gpu_raster_t *s, uint16_t *dst_row, const raster_env_t *e,
                                   const int *drow, int x, int r, int g, int b, int u, int v,
                                   int textured, int raw, int semi_flag, int use_dither,
                                   int tex_mode, int mask_check) {
    uint16_t texel = 0, out;
    int semi = semi_flag;
    if (mask_check && (dst_row[x] & 0x8000u)) return;
    if (textured) {
        texel = fetch_texel_fast(s, e, u, v, tex_mode);
        if (!texel) return;
        if (raw) {
            if (!semi_flag || !(texel & 0x8000u)) {
                dst_row[x] = (uint16_t)(texel | e->mask_set);
                return;
            }
            r = (int)(texel & 31u); g = (int)((texel >> 5) & 31u); b = (int)((texel >> 10) & 31u);
            dst_row[x] = (uint16_t)(blend_pixel(dst_row[x], r, g, b, e->blend_mode, 0x8000u) | e->mask_set);
            return;
        }
        r = (int)(((texel & 31u) * (unsigned)r) >> 4);
        g = (int)((((texel >> 5) & 31u) * (unsigned)g) >> 4);
        b = (int)((((texel >> 10) & 31u) * (unsigned)b) >> 4);
        semi = semi_flag && ((texel & 0x8000u) != 0);
    }
    if (use_dither) {
        int d = drow[x & 3];
        r = clamp(r + d, 255) >> 3; g = clamp(g + d, 255) >> 3; b = clamp(b + d, 255) >> 3;
    } else {
        r = clamp(r, 255) >> 3; g = clamp(g, 255) >> 3; b = clamp(b, 255) >> 3;
    }
    if (semi) {
        out = blend_pixel(dst_row[x], r, g, b, e->blend_mode, texel & 0x8000u);
    } else {
        out = (uint16_t)((unsigned)r | ((unsigned)g << 5) | ((unsigned)b << 10) | (texel & 0x8000u));
    }
    dst_row[x] = (uint16_t)(out | e->mask_set);
}

static inline int clip_edge_x(int e, int de, unsigned ade, uint64_t rde, int min_e, int l, int *lo, int *hi) {
    int need = min_e - e;
    if (de > 0) {
        if (need > 0) {
            int dx = (int)div_u32_recip((unsigned)(need + de - 1), ade, rde);
            if (l + dx > *lo) *lo = l + dx;
        }
    } else if (de < 0) {
        int allow = -need;
        if (allow < 0) return 0;
        {
            int dx = (int)div_u32_recip((unsigned)allow, ade, rde);
            if (l + dx < *hi) *hi = l + dx;
        }
    } else if (need > 0) {
        return 0;
    }
    return *lo <= *hi;
}

typedef struct {
    int flat, nonneg;
    int q, rem, dq, dr;
    int64_t num, dnum;
} attr_step_t;

static inline void attr_init_tri(attr_step_t *st, int av, int bv, int cv,
                                 int dea, int deb, int dec, int area, uint64_t reciprocal) {
    st->flat = (av == bv && av == cv);
    st->nonneg = (av >= 0 && bv >= 0 && cv >= 0);
    st->q = av; st->rem = 0; st->dq = 0; st->dr = 0;
    st->num = 0; st->dnum = 0;
    if (!st->flat) {
        int64_t dnum = (int64_t)dea * av + (int64_t)deb * bv + (int64_t)dec * cv;
        st->dnum = dnum;
        if (st->nonneg) {
            if (dnum >= 0) {
                divmod_u64_recip((uint64_t)dnum, (unsigned)area, reciprocal, &st->dq, &st->dr);
            } else {
                int mq, mr;
                divmod_u64_recip((uint64_t)(-dnum), (unsigned)area, reciprocal, &mq, &mr);
                if (mr) { st->dq = -mq - 1; st->dr = area - mr; }
                else { st->dq = -mq; st->dr = 0; }
            }
        }
    }
}

static inline void attr_init_row(attr_step_t *st, int av, int bv, int cv,
                                 int ea, int eb, int ec, int area, uint64_t reciprocal) {
    int64_t num;
    if (st->flat) return;
    num = (int64_t)ea * av + (int64_t)eb * bv + (int64_t)ec * cv;
    st->num = num;
    if (st->nonneg) {
        divmod_u64_recip((uint64_t)num, (unsigned)area, reciprocal, &st->q, &st->rem);
    } else {
        st->q = (int)(num / area);
    }
}

static inline void attr_advance_slow(attr_step_t *st, int area) {
    if (st->flat) return;
    if (st->nonneg) {
        st->q += st->dq;
        st->rem += st->dr;
        if (st->rem >= area) { st->rem -= area; ++st->q; }
    } else {
        st->num += st->dnum;
        st->q = (int)(st->num / area);
    }
}

#define STEP_ATTR(q, rem, dq, dr, area) do { \
    (q) += (dq); (rem) += (dr); \
    if ((rem) >= (area)) { (rem) -= (area); ++(q); } \
} while (0)

static inline void draw_tri_span_fast(gpu_raster_t *s, uint16_t *dst_row, const raster_env_t *env,
                                      const int *drow, int x0, int x1, int area,
                                      gpu_paint_t p,
                                      const attr_step_t *sr, const attr_step_t *sg, const attr_step_t *sb,
                                      const attr_step_t *su, const attr_step_t *sv) {
    int x;
    int rq = sr->q, rrem = sr->rem, rdq = sr->dq, rdr = sr->dr;
    int gq = sg->q, grem = sg->rem, gdq = sg->dq, gdr = sg->dr;
    int bq = sb->q, brem = sb->rem, bdq = sb->dq, bdr = sb->dr;
    int uq = su->q, urem = su->rem, udq = su->dq, udr = su->dr;
    int vq = sv->q, vrem = sv->rem, vdq = sv->dq, vdr = sv->dr;
    int mc = env->mask_check;
    if (!p.textured) {
        if (sr->flat && sg->flat && sb->flat && !env->use_dither) {
            int cr = clamp(rq, 255) >> 3, cg = clamp(gq, 255) >> 3, cb = clamp(bq, 255) >> 3;
            if (!p.semi && !mc) {
                uint16_t out = (uint16_t)((unsigned)cr | ((unsigned)cg << 5) | ((unsigned)cb << 10) | env->mask_set);
                for (x = x0; x <= x1; ++x) dst_row[x] = out;
            } else if (!p.semi) {
                uint16_t out = (uint16_t)((unsigned)cr | ((unsigned)cg << 5) | ((unsigned)cb << 10) | env->mask_set);
                for (x = x0; x <= x1; ++x) if (!(dst_row[x] & 0x8000u)) dst_row[x] = out;
            } else if (!mc) {
                for (x = x0; x <= x1; ++x)
                    dst_row[x] = (uint16_t)(blend_pixel(dst_row[x], cr, cg, cb, env->blend_mode, 0) | env->mask_set);
            } else {
                for (x = x0; x <= x1; ++x)
                    if (!(dst_row[x] & 0x8000u))
                        dst_row[x] = (uint16_t)(blend_pixel(dst_row[x], cr, cg, cb, env->blend_mode, 0) | env->mask_set);
            }
            return;
        }
        if (!p.semi && !env->use_dither) {
            for (x = x0; x <= x1; ++x) {
                emit_fast_pixel(s, dst_row, env, drow, x, rq, gq, bq, 0, 0, 0, 0, 0, 0, 0, mc);
                STEP_ATTR(rq, rrem, rdq, rdr, area);
                STEP_ATTR(gq, grem, gdq, gdr, area);
                STEP_ATTR(bq, brem, bdq, bdr, area);
            }
        } else if (!p.semi) {
            for (x = x0; x <= x1; ++x) {
                emit_fast_pixel(s, dst_row, env, drow, x, rq, gq, bq, 0, 0, 0, 0, 0, 1, 0, mc);
                STEP_ATTR(rq, rrem, rdq, rdr, area);
                STEP_ATTR(gq, grem, gdq, gdr, area);
                STEP_ATTR(bq, brem, bdq, bdr, area);
            }
        } else if (!env->use_dither) {
            for (x = x0; x <= x1; ++x) {
                emit_fast_pixel(s, dst_row, env, drow, x, rq, gq, bq, 0, 0, 0, 0, 1, 0, 0, mc);
                STEP_ATTR(rq, rrem, rdq, rdr, area);
                STEP_ATTR(gq, grem, gdq, gdr, area);
                STEP_ATTR(bq, brem, bdq, bdr, area);
            }
        } else {
            for (x = x0; x <= x1; ++x) {
                emit_fast_pixel(s, dst_row, env, drow, x, rq, gq, bq, 0, 0, 0, 0, 1, 1, 0, mc);
                STEP_ATTR(rq, rrem, rdq, rdr, area);
                STEP_ATTR(gq, grem, gdq, gdr, area);
                STEP_ATTR(bq, brem, bdq, bdr, area);
            }
        }
        return;
    }
    if (p.raw) {
        int tm = env->fast_tex;
        if (!p.semi && tm == 1 && !mc) {
            const uint16_t *tbase = env->tbase, *clut = env->clut_direct;
            uint16_t mset = env->mask_set;
            for (x = x0; x <= x1; ++x) {
                unsigned tx = (unsigned)uq & 255u, ty = (unsigned)vq & 255u;
                unsigned packed = tbase[ty * 1024u + (tx >> 2)];
                uint16_t texel = clut[(packed >> ((tx & 3u) * 4u)) & 15u];
                if (texel) dst_row[x] = (uint16_t)(texel | mset);
                STEP_ATTR(uq, urem, udq, udr, area);
                STEP_ATTR(vq, vrem, vdq, vdr, area);
            }
        } else if (!p.semi && tm == 2 && !mc) {
            const uint16_t *tbase = env->tbase, *clut = env->clut_direct;
            uint16_t mset = env->mask_set;
            for (x = x0; x <= x1; ++x) {
                unsigned tx = (unsigned)uq & 255u, ty = (unsigned)vq & 255u;
                unsigned packed = tbase[ty * 1024u + (tx >> 1)];
                uint16_t texel = clut[(packed >> ((tx & 1u) * 8u)) & 255u];
                if (texel) dst_row[x] = (uint16_t)(texel | mset);
                STEP_ATTR(uq, urem, udq, udr, area);
                STEP_ATTR(vq, vrem, vdq, vdr, area);
            }
        } else if (!p.semi && tm == 3 && !mc) {
            const uint16_t *tbase = env->tbase;
            uint16_t mset = env->mask_set;
            for (x = x0; x <= x1; ++x) {
                uint16_t texel = tbase[((unsigned)vq & 255u) * 1024u + ((unsigned)uq & 255u)];
                if (texel) dst_row[x] = (uint16_t)(texel | mset);
                STEP_ATTR(uq, urem, udq, udr, area);
                STEP_ATTR(vq, vrem, vdq, vdr, area);
            }
        } else if (p.semi && tm == 3 && !mc) {
            const uint16_t *tbase = env->tbase;
            uint16_t mset = env->mask_set;
            unsigned bmode = env->blend_mode;
            for (x = x0; x <= x1; ++x) {
                uint16_t texel = tbase[((unsigned)vq & 255u) * 1024u + ((unsigned)uq & 255u)];
                if (texel) {
                    if (texel & 0x8000u)
                        dst_row[x] = (uint16_t)(blend_pixel(dst_row[x], (int)(texel & 31u), (int)((texel >> 5) & 31u), (int)((texel >> 10) & 31u), bmode, 0x8000u) | mset);
                    else
                        dst_row[x] = (uint16_t)(texel | mset);
                }
                STEP_ATTR(uq, urem, udq, udr, area);
                STEP_ATTR(vq, vrem, vdq, vdr, area);
            }
        } else if (!p.semi) {
            for (x = x0; x <= x1; ++x) {
                emit_fast_pixel(s, dst_row, env, drow, x, 0, 0, 0, uq, vq, 1, 1, 0, 0, tm, mc);
                STEP_ATTR(uq, urem, udq, udr, area);
                STEP_ATTR(vq, vrem, vdq, vdr, area);
            }
        } else {
            for (x = x0; x <= x1; ++x) {
                emit_fast_pixel(s, dst_row, env, drow, x, 0, 0, 0, uq, vq, 1, 1, 1, 0, tm, mc);
                STEP_ATTR(uq, urem, udq, udr, area);
                STEP_ATTR(vq, vrem, vdq, vdr, area);
            }
        }
        return;
    }
    {
        int tm = env->fast_tex;
        int semi_f = p.semi != 0, dither_f = env->use_dither != 0;
        int flat_rgb = sr->flat && sg->flat && sb->flat;
        if (flat_rgb && !dither_f && !mc && tm == 3) {
            const uint16_t *tbase = env->tbase;
            uint16_t mset = env->mask_set;
            unsigned ur = (unsigned)rq, ug = (unsigned)gq, ub = (unsigned)bq;
            unsigned bmode = env->blend_mode;
            for (x = x0; x <= x1; ++x) {
                uint16_t texel = tbase[((unsigned)vq & 255u) * 1024u + ((unsigned)uq & 255u)];
                if (texel) {
                    int r = (int)(((texel & 31u) * ur) >> 4);
                    int g = (int)((((texel >> 5) & 31u) * ug) >> 4);
                    int b = (int)((((texel >> 10) & 31u) * ub) >> 4);
                    r = (r > 255 ? 255 : r) >> 3;
                    g = (g > 255 ? 255 : g) >> 3;
                    b = (b > 255 ? 255 : b) >> 3;
                    if (semi_f && (texel & 0x8000u))
                        dst_row[x] = (uint16_t)(blend_pixel(dst_row[x], r, g, b, bmode, 0x8000u) | mset);
                    else
                        dst_row[x] = (uint16_t)((unsigned)r | ((unsigned)g << 5) | ((unsigned)b << 10) | (texel & 0x8000u) | mset);
                }
                STEP_ATTR(uq, urem, udq, udr, area);
                STEP_ATTR(vq, vrem, vdq, vdr, area);
            }
            return;
        }
        if (flat_rgb && !dither_f && !mc && tm == 1 && semi_f) {
            const uint16_t *tbase = env->tbase, *clut = env->clut_direct;
            uint16_t mset = env->mask_set;
            unsigned ur = (unsigned)rq, ug = (unsigned)gq, ub = (unsigned)bq;
            unsigned bmode = env->blend_mode;
            for (x = x0; x <= x1; ++x) {
                unsigned tx = (unsigned)uq & 255u, ty = (unsigned)vq & 255u;
                unsigned packed = tbase[ty * 1024u + (tx >> 2)];
                uint16_t texel = clut[(packed >> ((tx & 3u) * 4u)) & 15u];
                if (texel) {
                    int r = (int)(((texel & 31u) * ur) >> 4);
                    int g = (int)((((texel >> 5) & 31u) * ug) >> 4);
                    int b = (int)((((texel >> 10) & 31u) * ub) >> 4);
                    r = (r > 255 ? 255 : r) >> 3;
                    g = (g > 255 ? 255 : g) >> 3;
                    b = (b > 255 ? 255 : b) >> 3;
                    if (texel & 0x8000u)
                        dst_row[x] = (uint16_t)(blend_pixel(dst_row[x], r, g, b, bmode, 0x8000u) | mset);
                    else
                        dst_row[x] = (uint16_t)((unsigned)r | ((unsigned)g << 5) | ((unsigned)b << 10) | mset);
                }
                STEP_ATTR(uq, urem, udq, udr, area);
                STEP_ATTR(vq, vrem, vdq, vdr, area);
            }
            return;
        }
        if (flat_rgb && !dither_f && !mc && tm == 2 && semi_f) {
            const uint16_t *tbase = env->tbase, *clut = env->clut_direct;
            uint16_t mset = env->mask_set;
            unsigned ur = (unsigned)rq, ug = (unsigned)gq, ub = (unsigned)bq;
            unsigned bmode = env->blend_mode;
            for (x = x0; x <= x1; ++x) {
                unsigned tx = (unsigned)uq & 255u, ty = (unsigned)vq & 255u;
                unsigned packed = tbase[ty * 1024u + (tx >> 1)];
                uint16_t texel = clut[(packed >> ((tx & 1u) * 8u)) & 255u];
                if (texel) {
                    int r = (int)(((texel & 31u) * ur) >> 4);
                    int g = (int)((((texel >> 5) & 31u) * ug) >> 4);
                    int b = (int)((((texel >> 10) & 31u) * ub) >> 4);
                    r = (r > 255 ? 255 : r) >> 3;
                    g = (g > 255 ? 255 : g) >> 3;
                    b = (b > 255 ? 255 : b) >> 3;
                    if (texel & 0x8000u)
                        dst_row[x] = (uint16_t)(blend_pixel(dst_row[x], r, g, b, bmode, 0x8000u) | mset);
                    else
                        dst_row[x] = (uint16_t)((unsigned)r | ((unsigned)g << 5) | ((unsigned)b << 10) | mset);
                }
                STEP_ATTR(uq, urem, udq, udr, area);
                STEP_ATTR(vq, vrem, vdq, vdr, area);
            }
            return;
        }
        if (!semi_f && !dither_f && tm == 1 && !mc) {
            const uint16_t *tbase = env->tbase, *clut = env->clut_direct;
            uint16_t mset = env->mask_set;
            for (x = x0; x <= x1; ++x) {
                unsigned tx = (unsigned)uq & 255u, ty = (unsigned)vq & 255u;
                unsigned packed = tbase[ty * 1024u + (tx >> 2)];
                uint16_t texel = clut[(packed >> ((tx & 3u) * 4u)) & 15u];
                if (texel) {
                    int r = (int)(((texel & 31u) * (unsigned)rq) >> 4);
                    int g = (int)((((texel >> 5) & 31u) * (unsigned)gq) >> 4);
                    int b = (int)((((texel >> 10) & 31u) * (unsigned)bq) >> 4);
                    r = (r > 255 ? 255 : r) >> 3;
                    g = (g > 255 ? 255 : g) >> 3;
                    b = (b > 255 ? 255 : b) >> 3;
                    dst_row[x] = (uint16_t)((unsigned)r | ((unsigned)g << 5) | ((unsigned)b << 10) | (texel & 0x8000u) | mset);
                }
                STEP_ATTR(rq, rrem, rdq, rdr, area);
                STEP_ATTR(gq, grem, gdq, gdr, area);
                STEP_ATTR(bq, brem, bdq, bdr, area);
                STEP_ATTR(uq, urem, udq, udr, area);
                STEP_ATTR(vq, vrem, vdq, vdr, area);
            }
        } else if (!semi_f && dither_f && tm == 1 && !mc) {
            const uint16_t *tbase = env->tbase, *clut = env->clut_direct;
            uint16_t mset = env->mask_set;
            for (x = x0; x <= x1; ++x) {
                unsigned tx = (unsigned)uq & 255u, ty = (unsigned)vq & 255u;
                unsigned packed = tbase[ty * 1024u + (tx >> 2)];
                uint16_t texel = clut[(packed >> ((tx & 3u) * 4u)) & 15u];
                if (texel) {
                    int d = drow[x & 3];
                    int r = clamp((int)(((texel & 31u) * (unsigned)rq) >> 4) + d, 255) >> 3;
                    int g = clamp((int)((((texel >> 5) & 31u) * (unsigned)gq) >> 4) + d, 255) >> 3;
                    int b = clamp((int)((((texel >> 10) & 31u) * (unsigned)bq) >> 4) + d, 255) >> 3;
                    dst_row[x] = (uint16_t)((unsigned)r | ((unsigned)g << 5) | ((unsigned)b << 10) | (texel & 0x8000u) | mset);
                }
                STEP_ATTR(rq, rrem, rdq, rdr, area);
                STEP_ATTR(gq, grem, gdq, gdr, area);
                STEP_ATTR(bq, brem, bdq, bdr, area);
                STEP_ATTR(uq, urem, udq, udr, area);
                STEP_ATTR(vq, vrem, vdq, vdr, area);
            }
        } else if (!semi_f && !dither_f && tm == 2 && !mc) {
            const uint16_t *tbase = env->tbase, *clut = env->clut_direct;
            uint16_t mset = env->mask_set;
            for (x = x0; x <= x1; ++x) {
                unsigned tx = (unsigned)uq & 255u, ty = (unsigned)vq & 255u;
                unsigned packed = tbase[ty * 1024u + (tx >> 1)];
                uint16_t texel = clut[(packed >> ((tx & 1u) * 8u)) & 255u];
                if (texel) {
                    int r = (int)(((texel & 31u) * (unsigned)rq) >> 4);
                    int g = (int)((((texel >> 5) & 31u) * (unsigned)gq) >> 4);
                    int b = (int)((((texel >> 10) & 31u) * (unsigned)bq) >> 4);
                    r = (r > 255 ? 255 : r) >> 3;
                    g = (g > 255 ? 255 : g) >> 3;
                    b = (b > 255 ? 255 : b) >> 3;
                    dst_row[x] = (uint16_t)((unsigned)r | ((unsigned)g << 5) | ((unsigned)b << 10) | (texel & 0x8000u) | mset);
                }
                STEP_ATTR(rq, rrem, rdq, rdr, area);
                STEP_ATTR(gq, grem, gdq, gdr, area);
                STEP_ATTR(bq, brem, bdq, bdr, area);
                STEP_ATTR(uq, urem, udq, udr, area);
                STEP_ATTR(vq, vrem, vdq, vdr, area);
            }
        } else if (!semi_f && dither_f && tm == 2 && !mc) {
            const uint16_t *tbase = env->tbase, *clut = env->clut_direct;
            uint16_t mset = env->mask_set;
            for (x = x0; x <= x1; ++x) {
                unsigned tx = (unsigned)uq & 255u, ty = (unsigned)vq & 255u;
                unsigned packed = tbase[ty * 1024u + (tx >> 1)];
                uint16_t texel = clut[(packed >> ((tx & 1u) * 8u)) & 255u];
                if (texel) {
                    int d = drow[x & 3];
                    int r = clamp((int)(((texel & 31u) * (unsigned)rq) >> 4) + d, 255) >> 3;
                    int g = clamp((int)((((texel >> 5) & 31u) * (unsigned)gq) >> 4) + d, 255) >> 3;
                    int b = clamp((int)((((texel >> 10) & 31u) * (unsigned)bq) >> 4) + d, 255) >> 3;
                    dst_row[x] = (uint16_t)((unsigned)r | ((unsigned)g << 5) | ((unsigned)b << 10) | (texel & 0x8000u) | mset);
                }
                STEP_ATTR(rq, rrem, rdq, rdr, area);
                STEP_ATTR(gq, grem, gdq, gdr, area);
                STEP_ATTR(bq, brem, bdq, bdr, area);
                STEP_ATTR(uq, urem, udq, udr, area);
                STEP_ATTR(vq, vrem, vdq, vdr, area);
            }
        } else if (!semi_f && !dither_f) {
            for (x = x0; x <= x1; ++x) {
                emit_fast_pixel(s, dst_row, env, drow, x, rq, gq, bq, uq, vq, 1, 0, 0, 0, tm, mc);
                STEP_ATTR(rq, rrem, rdq, rdr, area);
                STEP_ATTR(gq, grem, gdq, gdr, area);
                STEP_ATTR(bq, brem, bdq, bdr, area);
                STEP_ATTR(uq, urem, udq, udr, area);
                STEP_ATTR(vq, vrem, vdq, vdr, area);
            }
        } else if (!semi_f) {
            for (x = x0; x <= x1; ++x) {
                emit_fast_pixel(s, dst_row, env, drow, x, rq, gq, bq, uq, vq, 1, 0, 0, 1, tm, mc);
                STEP_ATTR(rq, rrem, rdq, rdr, area);
                STEP_ATTR(gq, grem, gdq, gdr, area);
                STEP_ATTR(bq, brem, bdq, bdr, area);
                STEP_ATTR(uq, urem, udq, udr, area);
                STEP_ATTR(vq, vrem, vdq, vdr, area);
            }
        } else if (!dither_f) {
            for (x = x0; x <= x1; ++x) {
                emit_fast_pixel(s, dst_row, env, drow, x, rq, gq, bq, uq, vq, 1, 0, 1, 0, tm, mc);
                STEP_ATTR(rq, rrem, rdq, rdr, area);
                STEP_ATTR(gq, grem, gdq, gdr, area);
                STEP_ATTR(bq, brem, bdq, bdr, area);
                STEP_ATTR(uq, urem, udq, udr, area);
                STEP_ATTR(vq, vrem, vdq, vdr, area);
            }
        } else {
            for (x = x0; x <= x1; ++x) {
                emit_fast_pixel(s, dst_row, env, drow, x, rq, gq, bq, uq, vq, 1, 0, 1, 1, tm, mc);
                STEP_ATTR(rq, rrem, rdq, rdr, area);
                STEP_ATTR(gq, grem, gdq, gdr, area);
                STEP_ATTR(bq, brem, bdq, bdr, area);
                STEP_ATTR(uq, urem, udq, udr, area);
                STEP_ATTR(vq, vrem, vdq, vdr, area);
            }
        }
    }
}

void gpu_raster_triangle(gpu_raster_t *s, gpu_vertex_t a, gpu_vertex_t b,
                         gpu_vertex_t c, gpu_paint_t p) {
    int l = min(a.x, min(b.x, c.x)), r = max(a.x, max(b.x, c.x));
    int t = min(a.y, min(b.y, c.y)), bot = max(a.y, max(b.y, c.y));
    int area, x, y, dea, deb, dec, dya, dyb, dyc, ta, tb, tc, min_ea, min_eb, min_ec;
    unsigned adea, adeb, adec;
    uint64_t reciprocal, rdea, rdeb, rdec;
    int ea_l, eb_l, ec_l;
    int need_rgb, in_bounds, all_nonneg, fast_mode;
    raster_env_t env;
    attr_step_t sr = {0}, sg = {0}, sb = {0}, su = {0}, sv = {0};
    if (r - l >= 1024 || bot - t >= 512) return;
    area = edge(a, b, c.x, c.y);
    if (!area) return;
    if (area < 0) { gpu_vertex_t tmp = b; b = c; c = tmp; area = -area; }
    l = max(l, s->left); r = min(r, s->right);
    t = max(t, s->top); bot = min(bot, s->bottom);
    if (l > r || t > bot) return;
    if (!edge_recip_ready) init_edge_recip();
    reciprocal = (UINT64_C(1) << 32) / (unsigned)area;
    ta = top_left(b, c); tb = top_left(c, a); tc = top_left(a, b);
    min_ea = ta ? 0 : 1; min_eb = tb ? 0 : 1; min_ec = tc ? 0 : 1;
    dea = b.y - c.y; deb = c.y - a.y; dec = a.y - b.y;
    dya = c.x - b.x; dyb = a.x - c.x; dyc = b.x - a.x;
    adea = (unsigned)(dea < 0 ? -dea : dea);
    adeb = (unsigned)(deb < 0 ? -deb : deb);
    adec = (unsigned)(dec < 0 ? -dec : dec);
    rdea = edge_recip[adea]; rdeb = edge_recip[adeb]; rdec = edge_recip[adec];
    need_rgb = !p.textured || !p.raw;
    raster_env_init(&env, s, p);
    in_bounds = (l >= 0 && r < 1024 && t >= 0 && bot < 512);
    all_nonneg = 1;
    if (need_rgb) {
        attr_init_tri(&sr, a.r, b.r, c.r, dea, deb, dec, area, reciprocal);
        attr_init_tri(&sg, a.g, b.g, c.g, dea, deb, dec, area, reciprocal);
        attr_init_tri(&sb, a.b, b.b, c.b, dea, deb, dec, area, reciprocal);
        all_nonneg &= sr.nonneg & sg.nonneg & sb.nonneg;
    } else {
        sr.q = a.r; sg.q = a.g; sb.q = a.b;
    }
    if (p.textured) {
        attr_init_tri(&su, a.u, b.u, c.u, dea, deb, dec, area, reciprocal);
        attr_init_tri(&sv, a.v, b.v, c.v, dea, deb, dec, area, reciprocal);
        all_nonneg &= su.nonneg & sv.nonneg;
    } else {
        su.q = 0; sv.q = 0;
    }
    fast_mode = in_bounds && all_nonneg && !env.has_fragment &&
                (!env.display_clip || s->interlaced);
    ea_l = edge(b, c, l, t); eb_l = edge(c, a, l, t); ec_l = edge(a, b, l, t);
    /* PS1 samples integer screen positions, not pixel centers at (x+.5,y+.5). */
    for (y = t; y <= bot; ++y, ea_l += dya, eb_l += dyb, ec_l += dyc) {
        int x0 = l, x1 = r;
        int ea, eb, ec;
        if (env.display_clip && s->interlaced && ((y & 1) == s->field)) continue;
        if (!clip_edge_x(ea_l, dea, adea, rdea, min_ea, l, &x0, &x1)) continue;
        if (!clip_edge_x(eb_l, deb, adeb, rdeb, min_eb, l, &x0, &x1)) continue;
        if (!clip_edge_x(ec_l, dec, adec, rdec, min_ec, l, &x0, &x1)) continue;
        ea = ea_l + dea * (x0 - l);
        eb = eb_l + deb * (x0 - l);
        ec = ec_l + dec * (x0 - l);
        if (need_rgb) {
            attr_init_row(&sr, a.r, b.r, c.r, ea, eb, ec, area, reciprocal);
            attr_init_row(&sg, a.g, b.g, c.g, ea, eb, ec, area, reciprocal);
            attr_init_row(&sb, a.b, b.b, c.b, ea, eb, ec, area, reciprocal);
        }
        if (p.textured) {
            attr_init_row(&su, a.u, b.u, c.u, ea, eb, ec, area, reciprocal);
            attr_init_row(&sv, a.v, b.v, c.v, ea, eb, ec, area, reciprocal);
        }
        if (fast_mode || (in_bounds && all_nonneg && !env.has_fragment &&
                          (((y - s->display_y) & 511) >= s->display_h))) {
            draw_tri_span_fast(s, &s->vram[y * 1024], &env, dither_table[y & 3],
                               x0, x1, area, p, &sr, &sg, &sb, &su, &sv);
        } else {
            for (x = x0; x <= x1; ++x) {
                pixel(s, x, y, sr.q, sg.q, sb.q, su.q, sv.q, p);
                if (need_rgb) {
                    attr_advance_slow(&sr, area); attr_advance_slow(&sg, area); attr_advance_slow(&sb, area);
                }
                if (p.textured) {
                    attr_advance_slow(&su, area); attr_advance_slow(&sv, area);
                }
            }
        }
    }
}

void gpu_raster_rectangle(gpu_raster_t *s, gpu_vertex_t a, int w, int h, gpu_paint_t p) {
    int l = max(a.x, s->left), r = min(a.x + w - 1, s->right);
    int t = max(a.y, s->top), b = min(a.y + h - 1, s->bottom), x, y;
    int du = (s->mode & 0x1000) ? -1 : 1, dv = (s->mode & 0x2000) ? -1 : 1;
    int in_bounds;
    raster_env_t env;
    if (l > r || t > b) return;
    raster_env_init(&env, s, p);
    in_bounds = (l >= 0 && r < 1024 && t >= 0 && b < 512);
    if (!in_bounds || env.has_fragment || a.r < 0 || a.g < 0 || a.b < 0 ||
        (env.display_clip && !s->interlaced)) {
        for (y = t; y <= b; ++y) for (x = l; x <= r; ++x)
            pixel(s, x, y, a.r, a.g, a.b, a.u + (x - a.x) * du, a.v + (y - a.y) * dv, p);
        return;
    }
    for (y = t; y <= b; ++y) {
        uint16_t *dst_row;
        const int *drow;
        int u, v, mc = env.mask_check, tm = env.fast_tex;
        if (env.display_clip && s->interlaced && ((y & 1) == s->field)) continue;
        dst_row = &s->vram[y * 1024];
        drow = dither_table[y & 3];
        if (!p.textured) {
            if (!env.use_dither) {
                int cr = clamp(a.r, 255) >> 3, cg = clamp(a.g, 255) >> 3, cb = clamp(a.b, 255) >> 3;
                if (!p.semi && !mc) {
                    uint16_t out = (uint16_t)((unsigned)cr | ((unsigned)cg << 5) | ((unsigned)cb << 10) | env.mask_set);
                    for (x = l; x <= r; ++x) dst_row[x] = out;
                } else if (!p.semi) {
                    uint16_t out = (uint16_t)((unsigned)cr | ((unsigned)cg << 5) | ((unsigned)cb << 10) | env.mask_set);
                    for (x = l; x <= r; ++x) if (!(dst_row[x] & 0x8000u)) dst_row[x] = out;
                } else if (!mc) {
                    for (x = l; x <= r; ++x)
                        dst_row[x] = (uint16_t)(blend_pixel(dst_row[x], cr, cg, cb, env.blend_mode, 0) | env.mask_set);
                } else {
                    for (x = l; x <= r; ++x)
                        if (!(dst_row[x] & 0x8000u))
                            dst_row[x] = (uint16_t)(blend_pixel(dst_row[x], cr, cg, cb, env.blend_mode, 0) | env.mask_set);
                }
            } else {
                for (x = l; x <= r; ++x)
                    emit_fast_pixel(s, dst_row, &env, drow, x, a.r, a.g, a.b, 0, 0, 0, 0, p.semi != 0, 1, 0, mc);
            }
        } else {
            u = a.u + (l - a.x) * du;
            v = a.v + (y - a.y) * dv;
            if (p.raw) {
                if (!p.semi) {
                    for (x = l; x <= r; ++x, u += du)
                        emit_fast_pixel(s, dst_row, &env, drow, x, 0, 0, 0, u, v, 1, 1, 0, 0, tm, mc);
                } else {
                    for (x = l; x <= r; ++x, u += du)
                        emit_fast_pixel(s, dst_row, &env, drow, x, 0, 0, 0, u, v, 1, 1, 1, 0, tm, mc);
                }
            } else {
                for (x = l; x <= r; ++x, u += du)
                    emit_fast_pixel(s, dst_row, &env, drow, x, a.r, a.g, a.b, u, v, 1, 0, p.semi != 0, env.use_dither != 0, tm, mc);
            }
        }
    }
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
    if (!s->mask && sx >= 0 && sy >= 0 && dx >= 0 && dy >= 0 &&
        sx + w <= 1024 && dx + w <= 1024 && sy + h <= 512 && dy + h <= 512) {
        for (y = 0; y < h; ++y)
            memmove(&s->vram[(dy + y) * 1024 + dx], &s->vram[(sy + y) * 1024 + sx], (size_t)w * 2u);
        return;
    }
    for (y = 0; y < h; ++y) {
        for (x = 0; x < w; ++x) row[x] = s->vram[((sy + y) & 511) * 1024 + ((sx + x) & 1023)];
        for (x = 0; x < w; ++x) gpu_raster_store(s, dx + x, dy + y, row[x]);
    }
}
