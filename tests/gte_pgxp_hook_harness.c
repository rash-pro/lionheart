#include "gte.h"
#include "pgxp.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

static unsigned projections;
void pgxp_record(const cpu_t *c, uint32_t xy, int64_t x, int64_t y, uint32_t z) {
    assert(xy == c->gte_data[14]);
    assert(x == 10 * 65536 && y == 20 * 65536 && z == 100);
    ++projections;
}

int main(void) {
    cpu_t *c = calloc(1, sizeof(*c));
    unsigned i;
    assert(c);
    c->gte_ctrl[0] = c->gte_ctrl[2] = c->gte_ctrl[4] = 4096;
    c->gte_ctrl[26] = 100;
    for (i = 0; i < 3; ++i) {
        c->gte_data[i * 2] = 10 | (20u << 16);
        c->gte_data[i * 2 + 1] = 100;
    }
    gte_cmd(c, (1u << 19) | 0x01);
    gte_cmd(c, (1u << 19) | 0x30);
    /* GTE_REFERENCE uses the generic dispatcher. The other builds use the
     * specialized RTPS/RTPT entries, including FF8_PLAY. */
#if defined(FF8_PGXP)
    assert(projections == 4);
#else
    assert(projections == 0);
#endif
    assert(fwrite(c->gte_data, sizeof(c->gte_data), 1, stdout) == 1);
    assert(fwrite(c->gte_ctrl, sizeof(c->gte_ctrl), 1, stdout) == 1);
    assert(fwrite(&c->cycles, sizeof(c->cycles), 1, stdout) == 1);
    free(c);
    return 0;
}
