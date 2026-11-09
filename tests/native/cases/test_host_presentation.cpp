// Fullscreen and final texture filtering through SDL's dummy video driver.
#include "host_fixtures.h"
LH_TEST(test_fullscreen_and_filter) {
    t.ok(lh::dummy(t.harness("presentation")).timeout(30));
}
