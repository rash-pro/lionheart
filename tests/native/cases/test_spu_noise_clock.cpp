// Noise frequency changes do not inherit a countdown at the old rate.
#include "lh_test.h"

static const char *const kBuilds[] = {"exact", "play"};

LH_TEST_P(test_noise_clock_transitions, kBuilds) {
    t.ok(t.harness(p).timeout(30));
}
