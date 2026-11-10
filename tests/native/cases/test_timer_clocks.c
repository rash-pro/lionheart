
#include "hw.h"
#include <assert.h>
#include <stdlib.h>

int main(void) {
    cpu_t *c = calloc(1, sizeof(*c));
    const uint64_t elapsed[] = {1, 7, 8, 65535, 65536, UINT32_MAX - 7u,
                               UINT32_MAX, UINT64_C(0x200000009)};
    unsigned timer, clock, display, n;
    assert(c); cpu_init(c); hw_get(c);
    for (timer = 0; timer < 3; ++timer)
    for (clock = 0; clock < 4; ++clock)
    for (display = 0; display < 5; ++display)
    for (n = 0; n < sizeof(elapsed) / sizeof(*elapsed); ++n) {
        hw_timer_t *t = &c->hw->timers[timer];
        uint64_t num = 1, den = 1, total, ticks;
        const unsigned divs[] = {10, 8, 5, 4, 7};
        uint32_t command = display == 4 ? 0x08000040 : 0x08000000 | display;
        gpu_gp1(c->hw->gpu, command);
        if (timer == 2 ? clock & 2 : clock & 1) {
            if (timer == 0) { num = 53693175; den = (uint64_t)HW_CPU_HZ * divs[display]; }
            else if (timer == 1) num = 0;
            else den = 8;
        }
        total = elapsed[n] * num;
        ticks = total / den;
        for (unsigned i = 0; i < 3; ++i) {
            c->hw->timers[i] = (hw_timer_t){0};
            c->hw->timers[i].mode = 1; /* stopped timer 2; harmless peers 0/1 */
        }
        *t = (hw_timer_t){0};
        t->mode = 0x400 | (clock << 8);
        t->target = 10;
        c->hw->now = 0;
        /* Materialization boundaries must not lose fractional clock ticks. */
        c->hw->now = elapsed[n] / 3; timers_advance(c, 0);
        c->hw->now = elapsed[n] / 2; timers_advance(c, 0);
        c->hw->now = elapsed[n]; timers_advance(c, 0);
        assert(t->cycle == elapsed[n]);
        assert(t->count == (uint16_t)ticks);
        assert(t->fraction == total % den);
        assert(!!(t->mode & 0x800) == (ticks >= 10));
        assert(!!(t->mode & 0x1000) == (ticks >= 65535));
    }
    hw_destroy(c); free(c); return 0;
}
