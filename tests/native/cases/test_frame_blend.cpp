// Frame blending on a synthetic double-buffered scene (tests/frame_blend_harness.c):
// two quads that share an edge, each its own transform run, moving and turning;
// a triangle with a corner the GTE never projected; a cut, a scene change and a
// gap. The harness records what it draws (runtime/frame_record.h), and the
// offline renderer (tools/devtools/inbetween.c) draws it again.
#include "lh_test.h"

namespace {
std::string scenario(lh::Case &t, const char *name, const lh::fs::path &record = {}) {
    auto command = t.harness("main").arg(name);
    if (!record.empty()) command.arg(record);
    return t.ok(command.timeout(60)).out;
}

std::string redraw(lh::Case &t, const lh::fs::path &record) {
    return t.ok(t.harness("inbetween").arg(record).timeout(120)).out;
}

} // namespace

LH_TEST(test_alpha_zero_and_one_are_the_two_game_frames) {
    auto out = scenario(t, "move");
    // Every corner moves; at alpha 1 it is where the game put it, at alpha 0
    // where its pair stood in the frame before.
    for (int f = 1; f <= 7; ++f) {
        LH_CONTAINS(out, "F" + std::to_string(f) + " reason=kept moving=8/8 polygons=2/2");
        LH_CONTAINS(out, "E" + std::to_string(f) + " ends=exact");
    }
    LH_CHECK_MSG(out.find("ends=differ") == std::string::npos, out);
    // Drawn again from the recording: alpha 1 is each game frame's picture
    // and, with everything blended, alpha 0 is the one before it.
    auto record = t.temp() / "move.rec";
    scenario(t, "move", record);
    auto drawn = redraw(t, record);
    LH_CONTAINS(drawn, "Pictures: replayed=16 differ=0");
    LH_CONTAINS(drawn, "Alpha 1: game-frames=6 differ=0 pixels=0");
    LH_CONTAINS(drawn, "Alpha 0: game-frames-equal-to-previous=6");
    LH_CONTAINS(drawn, "Game rate: changed=");
    LH_CONTAINS(drawn, " jumped=0 ");
    LH_CONTAINS(drawn, "Stray: corners=0");
}

LH_TEST(test_shared_vertices_stay_shared) {
    // The quads share two positions through two projections each; halfway
    // they must still meet exactly.
    auto out = scenario(t, "move");
    for (int f = 0; f <= 7; ++f) LH_CONTAINS(out, "E" + std::to_string(f) + " ends=exact shared=kept");
}

LH_TEST(test_a_cut_resets) {
    auto out = scenario(t, "resets");
    LH_CONTAINS(out, "F2 reason=kept moving=8/8");
    LH_CONTAINS(out, "F3 reason=cut moving=0/8 polygons=0/2 held=0/0/0/8\nM3 A=0 B=0");
    LH_CONTAINS(out, "F4 reason=kept moving=8/8");
    LH_CONTAINS(out, "F5 reason=scene moving=0/8");
    LH_CONTAINS(out, "F6 reason=kept moving=8/8");
    LH_CONTAINS(out, "F7 reason=no-history moving=0/8");
    LH_CONTAINS(out, "F8 reason=kept moving=8/8");
}

LH_TEST(test_held_corners_do_not_move) {
    // C has a corner no projection made: it is held, and so are B's two
    // corners at its positions; A and B's other corners move.
    auto out = scenario(t, "held");
    for (int f = 1; f <= 7; ++f) {
        LH_CONTAINS(out, "F" + std::to_string(f) + " reason=kept moving=6/11 polygons=1/3 held=3/0/2/0");
        LH_CONTAINS(out, "M" + std::to_string(f) + " A=f B=5 C=0");
    }
    auto record = t.temp() / "held.rec";
    scenario(t, "held", record);
    auto drawn = redraw(t, record);
    LH_CONTAINS(drawn, "Alpha 1: game-frames=6 differ=0 pixels=0");
    // What is held changes only at game frames, and the measure says so.
    LH_CONTAINS(drawn, "Alpha 0: game-frames-equal-to-previous=0");
    auto jumped = lh::capture(drawn, R"(jumped=(\d+) )");
    LH_CHECK_MSG(jumped != "0", drawn);
}
