// Runtime projection publication on exact, play and generic GTE paths.
#include "lh_test.h"

#include <string>

static const char *const kModes[] = {"exact", "play", "generic"};

LH_TEST_P(test_projection_hook_preserves_gte_and_publishes_each_fifo_push, kModes) {
    const std::string mode = p;
    const auto disabled = t.ok(t.harness(mode + ".off"));
    const auto enabled = t.ok(t.harness(mode + ".on"));
    LH_BYTES_EQ(enabled.out, disabled.out);
}
