
#include "hw.h"
#include <assert.h>
#include <stdlib.h>

int main(void) {
    for (unsigned observer = 0; observer < 3; ++observer) {
        cpu_t *c = calloc(1, sizeof(*c));
        assert(c); cpu_init(c); hw_get(c);
        hw_write(c, 0x1f801824, 4, 0xe0000000);
        hw_write(c, 0x1f801820, 4, 0x24000004);
        for (unsigned i = 0; i < 4; ++i)
            hw_write(c, 0x1f801820, 4, 0xfe000000);
        hw_write(c, 0x1f8010f0, 4, 0x88);
        hw_write(c, 0x1f801090, 4, 0x180000);
        hw_write(c, 0x1f801094, 4, 0x10020);
        hw_write(c, 0x1f801098, 4, 0x01000200);
        assert(c->hw->media_bound != HW_NEVER);
        assert(c->next_event > c->cycles);
        code_watch(c, 0x80180000, 4);
        if (observer == 0) hw_event(c);
        else hw_write(c, observer == 1 ? 0x1f801810 : 0x1f801814,
                      4, observer == 1 ? 0 : 0x05000000);
        /* Watched destinations must return to the per-word calendar. */
        assert(c->hw->media_bound == HW_NEVER);
        assert(c->ram_top == 0x800000);
        assert(c->next_event == c->hw->next_device);
        hw_destroy(c); free(c);
    }
    return 0;
}
