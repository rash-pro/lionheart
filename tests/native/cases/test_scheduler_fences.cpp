// Observer scheduling must retire a media fence when its RAM becomes code.
#include "lh_test.h"

LH_TEST(test_scheduler_observers_recheck_new_code_watches) {
    t.ok(t.harness("fences").timeout(30));
}
