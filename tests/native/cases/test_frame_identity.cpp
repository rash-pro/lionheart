// Frame identity: transform runs, their keys and pairs, the repeat guard, resets
// and corner links, on synthetic projections and packets (tests/frame_identity_harness.c);
// and, with the owner's discs, the service on reference routes judged by the
// interpreter's provenance oracle.
#include "lh_test.h"

namespace {
std::string scenario(lh::Case &t, const char *name) {
    auto result = t.ok(t.harness("main").arg(name).timeout(60));
    return result.out;
}
} // namespace

LH_TEST(test_runs_are_consecutive_projections_under_one_setup) {
    auto out = scenario(t, "runs");
    // A triangle and two points under two setups, then the first setup again: three runs.
    LH_CONTAINS(out, "F0 flags=R runs=3 projections=6");
    LH_CONTAINS(out, "R0 first=0 count=3 previous=-");
    LH_CONTAINS(out, "R1 first=3 count=2 previous=-");
    LH_CONTAINS(out, "R2 first=5 count=1 previous=-");
}

LH_TEST(test_runs_pair_by_exact_inputs_and_vertex_k_with_vertex_k) {
    auto out = scenario(t, "runs");
    // Moved but unchanged parts pair; vertex k with vertex k.
    LH_CONTAINS(out, "F1 flags=- runs=3");
    LH_CONTAINS(out, "R0 first=0 count=3 previous=0");
    LH_CONTAINS(out, "R1 first=3 count=2 previous=1");
    LH_CONTAINS(out, "P4 run=1 previous=4 match=4");
    // One changed input, or one more vertex, is another key: held.
    LH_CONTAINS(out, "R2 first=5 count=1 previous=- repeats=0\nP0");
    LH_CONTAINS(out, "F2 flags=- runs=2 projections=6");
    LH_CONTAINS(out, "R1 first=3 count=3 previous=- repeats=0");
}

LH_TEST(test_repeated_keys_pair_only_with_the_mutually_nearest) {
    auto out = scenario(t, "repeats");
    LH_CONTAINS(out, "F1 flags=- runs=3 projections=6 polygons=0 vblank=2\n"
                     "R0 first=0 count=2 previous=0 repeats=2\n"
                     "R1 first=2 count=2 previous=1 repeats=2\n"
                     "R2 first=4 count=2 previous=2 repeats=2");
    // Instances drawn in another order still pair with themselves.
    LH_CONTAINS(out, "F2 flags=- runs=3 projections=6 polygons=0 vblank=4\n"
                     "R0 first=0 count=2 previous=2 repeats=2\n"
                     "R1 first=2 count=2 previous=1 repeats=2\n"
                     "R2 first=4 count=2 previous=0 repeats=2");
    // Two candidates at nearly the same distance: neither is clearly nearest.
    LH_CONTAINS(out, "F3 flags=- runs=2 projections=4 polygons=0 vblank=6\n"
                     "R0 first=0 count=2 previous=- repeats=1\n"
                     "R1 first=2 count=2 previous=- repeats=1");
}

LH_TEST(test_repeated_keys_beyond_the_screen_guard_are_held) {
    auto guarded = scenario(t, "repeats"), open = scenario(t, "repeats-unguarded");
    // Clearly nearest, but 25 pixels away on screen: held under the 8-pixel guard.
    LH_CONTAINS(guarded, "F5 flags=- runs=2 projections=4 polygons=0 vblank=10\n"
                         "R0 first=0 count=2 previous=0 repeats=1\n"
                         "R1 first=2 count=2 previous=- repeats=1");
    LH_CONTAINS(open, "F5 flags=- runs=2 projections=4 polygons=0 vblank=10\n"
                      "R0 first=0 count=2 previous=0 repeats=1\n"
                      "R1 first=2 count=2 previous=1 repeats=1");
}

LH_TEST(test_display_start_changes_end_frames_and_long_gaps_reset) {
    auto out = scenario(t, "gaps");
    LH_CONTAINS(out, "F0 flags=R");
    // Eight VBlanks between two ends keeps history; nine resets it.
    LH_CONTAINS(out, "F1 flags=- runs=1 projections=1 polygons=0 vblank=8\nR0 first=0 count=1 previous=0");
    LH_CONTAINS(out, "F2 flags=R runs=1 projections=1 polygons=0 vblank=17\nR0 first=0 count=1 previous=-");
    // Writing the same start again ends nothing.
    LH_CONTAINS(out, "F3 flags=- runs=1 projections=1 polygons=0 vblank=18\nR0 first=0 count=1 previous=0");
    // After a reset the first write only learns the start; the next frame has no history.
    LH_CONTAINS(out, "F4 flags=R runs=1 projections=2 polygons=0 vblank=20");
    LH_CHECK(out.find("F5") == std::string::npos);
}

LH_TEST(test_corners_link_to_the_newest_frame_holding_all_of_them) {
    auto out = scenario(t, "corners");
    LH_CONTAINS(out, "F2 flags=- runs=2 projections=7 polygons=6 vblank=4\n"
                     "R0 first=0 count=6 previous=0 repeats=0\n"
                     "R1 first=6 count=1 previous=- repeats=0");
    LH_CONTAINS(out, "Q0 L1:0 L1:1 L1:2\n");      // drawn after the change from the previous frame's table
    LH_CONTAINS(out, "Q1 L0:0 L0:1 L0:2\n");      // gouraud: this frame's own
    LH_CONTAINS(out, "Q2 L0:3 L0:4 L0:5 L0:5\n"); // textured quad
    LH_CONTAINS(out, "Q3 N N N\n");               // 2D: no projection there
    LH_CONTAINS(out, "Q4 S S S\n");               // no one frame holds every corner
    LH_CONTAINS(out, "Q5 L0:0 L0:1 U\n");         // a corner from a run without history
    // Drawn before this frame's projections: they cannot have made it.
    LH_CONTAINS(out, "F3 flags=- runs=1 projections=3 polygons=1 vblank=6\n"
                     "R0 first=0 count=3 previous=- repeats=0\n"
                     "P0 run=0 previous=- match=-\nP1 run=0 previous=- match=-\nP2 run=0 previous=- match=-\n"
                     "Q0 L1:0 L1:1 L1:2\n");
}

LH_TEST(test_shared_screen_points_are_decided_by_the_polygons_run_or_held) {
    auto out = scenario(t, "shared");
    // Two runs meet at two points that moved together: safe.
    LH_CONTAINS(out, "F1 flags=- runs=2 projections=6 polygons=1 vblank=2\n"
                     "R0 first=0 count=3 previous=0 repeats=0\nR1 first=3 count=3 previous=1 repeats=0\n");
    LH_CONTAINS(out, "P5 run=1 previous=5 match=5\nQ0 L0:0 L0:1 L0:2\nF2");
    // They stood apart before: the only run holding every corner decides; with
    // two such runs the corners are held.
    LH_CONTAINS(out, "P5 run=1 previous=5 match=5\nQ0 L0:0 L0:1 L0:2\nQ1 A A A\nQ2 S S S\n");
}

LH_TEST(test_excluded_frames_publish_no_pairs_and_no_links) {
    auto out = scenario(t, "excluded");
    LH_CONTAINS(out, "F1 flags=E runs=1 projections=3 polygons=1 vblank=2\n"
                     "R0 first=0 count=3 previous=- repeats=0\n"
                     "P0 run=0 previous=- match=0\n");
    LH_CONTAINS(out, "Q0 X0:0 X0:1 X0:2\n");
    // The next frame pairs with the excluded one as usual.
    LH_CONTAINS(out, "F2 flags=- runs=1 projections=3 polygons=1 vblank=4\n"
                     "R0 first=0 count=3 previous=0 repeats=0\n"
                     "P0 run=0 previous=0 match=0\n");
    LH_CONTAINS(out, "Q0 L0:0 L0:1 L0:2\n");
}

LH_TEST(test_points_the_cpu_moves_pair_by_the_step_most_shapes_agree_on) {
    auto out = scenario(t, "points");
    // No run keeps its inputs, but every point moved by one step: each pairs
    // with the point at its input less the step, vertex for vertex.
    LH_CONTAINS(out, "F1 flags=- runs=1 projections=75 polygons=0 vblank=2\n"
                     "S step=8,0,-12 votes=73\n"
                     "R0 first=0 count=75 previous=- repeats=0\n"
                     "P0 run=0 previous=0 match=0\nP1 run=0 previous=1 match=1\n");
    // A block that comes in has no record in the frame before: it is placed,
    // at its input less the step.
    LH_CONTAINS(out, "S step=5,1,3 votes=60\n");
    LH_CONTAINS(out, "P74 run=0 previous=74 match=74\nP75 run=0 previous=placed match=placed\n");
}

LH_TEST(test_a_corner_where_places_share_a_pixel_takes_the_place_nearest_its_polygon) {
    auto out = scenario(t, "copies");
    // A copy of a triangle twice as far stands at its pixels; the copy is
    // placed, the triangle pairs, and its polygon takes the triangle's corners.
    LH_CONTAINS(out, "P75 run=0 previous=placed match=placed\n");
    LH_CONTAINS(out, "P78 run=0 previous=78 match=78\n");
    LH_CONTAINS(out, "Q0 L0:78 L0:79 L0:80\n");
}

LH_TEST(test_a_place_kept_at_two_view_positions_pairs_nothing) {
    auto out = scenario(t, "points");
    LH_CONTAINS(out, "F4 flags=- runs=1 projections=75 polygons=0 vblank=8\n"
                     "S step=4,0,0 votes=73\n"
                     "R0 first=0 count=75 previous=- repeats=0\n"
                     "P0 run=0 previous=- match=-\nP1 run=0 previous=1 match=1\n");
}

LH_TEST(test_points_pair_by_a_step_only_with_enough_votes_and_the_hosts_word) {
    auto out = scenario(t, "points");
    LH_CONTAINS(out, "F5 flags=- runs=1 projections=6 polygons=0 vblank=10\n"
                     "S step=0,0,0 votes=0\n"
                     "R0 first=0 count=6 previous=- repeats=0\n"
                     "P0 run=0 previous=- match=-\n");
    LH_CONTAINS(out, "F6 flags=- runs=1 projections=75 polygons=0 vblank=12\n"
                     "R0 first=0 count=75 previous=- repeats=0\n"
                     "P0 run=0 previous=- match=-\n");
}

LH_TEST(test_recording_entries_compute_what_the_plain_ones_do) {
    LH_CONTAINS(scenario(t, "entries"), "entries match");
}
