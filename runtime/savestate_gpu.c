#include <stddef.h>
#ifdef FF8_SS_PRIVATE
#include "savestate_internal.h"
void ss_gpu(ss_io *io, gpu_t *g) {
    gpu_raster_t *r = &g->raster;
    ss_version(io, 1);
    S16(r->vram); S32(r->mode); S32(r->window); S32(r->mask);
    S32(r->left); S32(r->top); S32(r->right); S32(r->bottom);
    S32(r->display_x); S32(r->display_y); S32(r->display_w); S32(r->display_h);
    S32(r->display_enabled); S32(r->interlaced); S32(r->field);
    S32(g->packet); S32(g->read_latch); S32(g->draw_start); S32(g->draw_end); S32(g->draw_offset);
    S32(g->count); S32(g->needed); S32(g->opcode); S32(g->offset_x); S32(g->offset_y);
    S32(g->transfer); S32(g->tx); S32(g->ty); S32(g->tw); S32(g->th); S32(g->cursor);
    S32(g->polyline); S32(g->poly_color_pending); S32(g->poly_color);
    S32(g->poly_last.x); S32(g->poly_last.y); S32(g->poly_last.r); S32(g->poly_last.g); S32(g->poly_last.b);
    S32(g->poly_last.u); S32(g->poly_last.v);
    S32(g->poly_paint.textured); S32(g->poly_paint.raw); S32(g->poly_paint.semi); S32(g->poly_paint.dither); S32(g->poly_paint.clut);
    S32(g->display_start); S32(g->horizontal); S32(g->vertical); S32(g->display_mode);
    S32(g->disabled); S32(g->dma); S32(g->irq); S32(g->field);
    if (g->count > 12 || g->needed > 12 || g->transfer > 2 || g->tw > 1024 || g->th > 512 ||
        g->tx > 1023 || g->ty > 511 || (g->transfer && (!g->tw || !g->th)) || g->cursor > 1024 * 512 ||
        r->left < 0 || r->top < 0 || r->right > 1023 || r->bottom > 511) io->failed = 1;
}
#endif
