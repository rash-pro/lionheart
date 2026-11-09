// Cached loop proofs expire on every guest operation that can invalidate them.
#include "lh_test.h"

LH_TEST(test_loop_cache_invalidation) {
    t.ok(t.harness("main").timeout(60));
}
