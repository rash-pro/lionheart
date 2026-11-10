// Native PMON metadata stays current without replaying a register-read backlog.
#include "lh_test.h"

LH_TEST(test_live_pitch_preserves_guest_reads_dma_and_irq) {
    t.ok(t.harness("main").timeout(90));
}
