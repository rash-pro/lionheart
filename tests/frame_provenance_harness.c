#include "cpu.h"
#include "gte.h"
#include "hw.h"
#include "frame_provenance.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>

static unsigned corner;
void code_invalidate(cpu_t *c, uint32_t p, size_t n) { (void)c; (void)p; (void)n; abort(); }
void pgxp_record(const cpu_t *c, uint32_t key, int64_t x, int64_t y, uint32_t z) {
    (void)x; (void)y; (void)z;
    frame_provenance_projection(c, 10, key, 0x2000 + corner * 8, corner, corner);
    ++corner;
}
int main(int argc, char **argv) {
    cpu_t *c = calloc(1, sizeof(*c)), *before = calloc(1, sizeof(*c));
    assert(c && before && argc == 2);
    c->pc = 0x80001000; c->r[4] = 0x80003000;
    c->gte_ctrl[0] = c->gte_ctrl[2] = c->gte_ctrl[4] = 4096;
    c->gte_ctrl[26] = 100;
    for (unsigned i = 0; i < 3; ++i) {
        c->gte_data[2 * i] = 10; c->gte_data[2 * i + 1] = 100;
    }
    gte_rtpt(c, 12, 0);
    uint32_t key = c->gte_data[12];
    assert(key == c->gte_data[13] && key == c->gte_data[14]);
    /* Same SXY from three distinct projections: the FIFO, not latest SXY,
     * determines which observation SWC2 exports. */
    unsigned reg = !strcmp(argv[1], "alias") ? 15 : 12;
    ram_write(c, 0x1000, 4, (58u << 26) | (4u << 21) | (reg << 16));
    if (!strcmp(argv[1], "invalidate")) frame_provenance_write(reg, 0);
    memcpy(before, c, sizeof(*c));
    uint32_t result = gte_read(c, reg, 0);
    assert(result == key && !memcmp(before, c, sizeof(*c)));
    ram_write(c, 0x3000, 4, result);
    if (!strcmp(argv[1], "overwrite")) ram_write(c, 0x3000, 4, result ^ 1);
    if (!strcmp(argv[1], "same_value_reuse")) {
        ram_write(c, 0x3000, 4, 0); ram_write(c, 0x3000, 4, result);
    }
    memcpy(before, c, sizeof(*c));
    frame_provenance_dma(c, 11, c->ram + 0x3000, 1, 100);
    assert(!memcmp(before, c, sizeof(*c)));
    if (!strcmp(argv[1], "revision")) {
        c->gte_data[0] = 20; corner = 0; gte_rtpt(c, 12, 0);
    }
    free(before); free(c);
    return 0;
}
