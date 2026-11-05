/* Private diagnostic parser; uses the authoritative GP0 transfer rules. */
#include "../runtime/gpu.c"
#include <string.h>

unsigned frame_trace_size(void) { return sizeof(gpu_t); }
void frame_trace_free(gpu_t *g) { free(g); }
void frame_trace_restore(gpu_t *g, const void *p) { memcpy(g, p, sizeof(*g)); }
unsigned frame_trace_word(gpu_t *g, uint32_t word, uint32_t *out) {
    unsigned complete = !g->transfer && !g->polyline &&
        (g->count ? g->count + 1 == g->needed : packet_size(word >> 24) == 1);
    unsigned n = g->count + 1;
    gpu_gp0(g, word);
    if (!complete) return 0;
    memcpy(out, g->packet, n * sizeof(*out));
    return n;
}
