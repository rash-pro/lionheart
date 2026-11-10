// Shadow replay spans preserve PCM dependencies and mutable RAM aliases.
#include "lh_test.h"

static const char *const kModes[] = {"False", "True"};

LH_TEST_P(test_replay_dependencies, kModes) {
    t.ok(t.harness(p).timeout(90));
}
