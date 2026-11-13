#include "lh_test.h"

LH_TEST(test_a_collapsing_terrain_triangle_keeps_the_patch_closed) {
    auto result = t.ok(t.harness("main").timeout(60));
    LH_CONTAINS(result.out, "holes=0 endpoint-extras=0");
}

LH_TEST(test_a_terrain_triangle_is_visible_only_while_facing_the_viewer) {
    auto result = t.ok(t.harness("main").arg("reversed").timeout(60));
    LH_CONTAINS(result.out, "holes=0 endpoint-extras=0");
}

LH_TEST(test_outgoing_terrain_needs_no_history_before_its_older_endpoint) {
    auto result = t.ok(t.harness("main").arg("arriving").timeout(60));
    LH_CONTAINS(result.out, "holes=0 endpoint-extras=0");
}

LH_TEST(test_terrain_continuation_follows_camera_rotation_and_projection_changes) {
    auto result = t.ok(t.harness("main").arg("turn").timeout(60));
    LH_CONTAINS(result.out, "holes=0 endpoint-extras=0");
}

LH_TEST(test_incoming_terrain_does_not_show_its_back_face) {
    auto result = t.ok(t.harness("main").arg("incoming").timeout(60));
    LH_CONTAINS(result.out, "excess=0");
}

LH_TEST_P(test_continuation_rejects_a_changed_texture_or_drawing_state,
          (std::vector<std::string>{"texture", "palette", "window", "target"})) {
    auto result = t.ok(t.harness("main").arg(p).timeout(60));
    LH_CONTAINS(result.out, "unsafe-continuations=0");
}
