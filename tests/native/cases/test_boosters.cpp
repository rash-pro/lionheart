// Remaster-style gameplay boosters, 3x audio speed, and save-state taint propagation.
#include "host_fixtures.h"

LH_TEST(test_boosters_ram_savestate_and_audio_speed) {
    auto state = t.temp() / "tainted.ff8s";
    auto csv = t.temp() / "audio.csv";
    auto r = t.ok(lh::dummy(t.harness("boosters")).args({state, csv}).timeout(30));
    LH_CONTAINS(r.out, "ok");
    LH_CHECK(lh::fs::is_regular_file(state));
    LH_CHECK(lh::fs::is_regular_file(csv));
}
