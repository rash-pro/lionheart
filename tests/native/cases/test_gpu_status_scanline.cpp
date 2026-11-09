// GPUSTAT line parity must release a VSync wait across a display-mode change.
#include "lh_test.h"

static const char *const kModes[] = {"ntsc", "pal", "480i", "transition"};

LH_TEST_P(test_status_parity, kModes) {
    t.ok(t.harness("status").arg(p).timeout(15));
}
