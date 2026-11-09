// Play decoder memo and repeated-block spans match independent scalar output.
#include "lh_test.h"

static const char *const kFixtures[] = {"spu_decode_memo_harness", "spu_decode_loop_harness"};

LH_TEST_P(test_decode_memo, kFixtures) {
    t.ok(t.harness(p).timeout(90));
}
