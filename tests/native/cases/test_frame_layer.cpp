// Frame layers on a synthetic field (tests/frame_layer_harness.c): a
// background of tiles scrolled by the drawing offset, a quad the GTE projects
// walking across it, a tile drawn over the quad (a mask), and each scenario's
// own tiles: one animated, one that moves by itself, a layer of its own, a
// field that changes, tiles that do not move together, a background with no
// tile to spare at the edge, and a gap. The harness records what it draws
// (runtime/frame_record.h), and the offline renderer
// (tools/devtools/inbetween.c) draws it again.
#include "lh_test.h"

namespace {
namespace fs = lh::fs;

std::string scenario(lh::Case &t, const char *name, const fs::path &record = {}) {
    auto command = t.harness("main").arg(name);
    if (!record.empty()) command.arg(record);
    return t.ok(command.timeout(60)).out;
}

std::string redraw(lh::Case &t, const char *name) {
    auto record = t.temp() / (std::string(name) + ".rec");
    scenario(t, name, record);
    return t.ok(t.harness("inbetween").arg(record).arg("--causes").timeout(120)).out;
}

std::string causes(const std::string &drawn) {
    return lh::capture(drawn, R"(Causes \(share of the pixels that change only at game frames\):([^\n]*))");
}

void moves_every_frame(const std::string &out, const char *move = "move=-6,0") {
    for (int f = 1; f <= 7; ++f) {
        LH_CONTAINS(out, "L" + std::to_string(f) + " reason=moved " + move);
        LH_CONTAINS(out, "A" + std::to_string(f) + " layer=ok mask=ok relative=ok special=ok");
    }
}

} // namespace

LH_TEST(test_the_layer_moves_by_the_fraction_of_the_scroll) {
    // The offset scrolls 6 pixels a game frame; at each quarter of the
    // interval every tile stands at that fraction of its move.
    moves_every_frame(scenario(t, "scroll"));
    // Drawn again: alpha 1 is each game frame, and with everything moving,
    // alpha 0 is the one before: nothing changes only at game frames.
    auto drawn = redraw(t, "scroll");
    LH_CONTAINS(drawn, "Alpha 1: game-frames=6 differ=0 pixels=0");
    LH_CONTAINS(drawn, "Alpha 0: game-frames-equal-to-previous=6");
    LH_CONTAINS(drawn, " jumped=0 ");
    LH_CONTAINS(drawn, "Slide (3D corners that move under an offset whose layer holds while the main one moves): corners=0 ");
}

LH_TEST(test_masks_move_with_their_layer) {
    // The mask is drawn after the quad it hides; it is a tile like the others
    // and stays in its place in the picture.
    auto out = scenario(t, "scroll");
    for (int f = 1; f <= 7; ++f) LH_CONTAINS(out, "A" + std::to_string(f) + " layer=ok mask=ok");
    LH_CHECK_MSG(out.find("mask=differ") == std::string::npos, out);
}

LH_TEST(test_3d_and_background_keep_their_relative_position) {
    // The quad walks 6 pixels a frame in the picture while the scroll follows
    // it: on screen it stays, the background passes under it, and at every
    // fraction each corner stands at that fraction of its move against the
    // tile under it.
    for (auto name : {"scroll", "parallax", "animated", "disagree"}) {
        auto out = scenario(t, name);
        LH_CHECK_MSG(out.find("relative=differ") == std::string::npos, out);
        LH_CONTAINS(out, "A7 layer=ok mask=ok relative=ok");
    }
}

LH_TEST(test_an_animated_tile_moves_with_its_layer) {
    // One tile draws another part of the texture every frame, in its place:
    // it has no pair, moves with the layer and shows the newer picture.
    auto out = scenario(t, "animated");
    moves_every_frame(out);
    LH_CONTAINS(out, "L2 reason=moved move=-6,0 tiles=369 paired=368 unpaired=1 disagree=0");
    auto drawn = redraw(t, "animated");
    LH_CONTAINS(drawn, "Alpha 1: game-frames=6 differ=0 pixels=0");
    // What changes only at game frames is that tile's picture, nothing else.
    LH_EQ(causes(drawn), std::string(" sprite-moved-look-changed=100.00%"));
}

LH_TEST(test_a_tile_that_moves_by_itself_holds) {
    // One tile moves 5 pixels a frame in the picture against the layer's 0:
    // it stays where the newer frame has it while the layer moves.
    auto out = scenario(t, "disagree");
    moves_every_frame(out);
    LH_CONTAINS(out, "L2 reason=moved move=-6,0 tiles=370 paired=369 unpaired=0 disagree=1");
    auto drawn = redraw(t, "disagree");
    LH_CONTAINS(drawn, "Alpha 1: game-frames=6 differ=0 pixels=0");
    LH_CHECK_MSG(causes(drawn).find(" sprite=") != std::string::npos, drawn);
}

LH_TEST(test_a_layer_of_its_own_moves_at_its_own_rate) {
    // Ten tiles move 3 pixels a frame in the picture: on screen they move
    // half as fast as the rest, a layer of their own.
    auto out = scenario(t, "parallax");
    moves_every_frame(out);
    LH_CONTAINS(out, "L2 reason=moved move=-6,0 tiles=379 paired=379 unpaired=0 disagree=0 untextured=0 layers=2");
    auto drawn = redraw(t, "parallax");
    LH_CONTAINS(drawn, "Alpha 0: game-frames-equal-to-previous=6");
    LH_CONTAINS(drawn, " jumped=0 ");
}

LH_TEST(test_a_field_change_resets) {
    // From the fifth frame every tile is another: the frame that changes holds,
    // the next moves again.
    auto out = scenario(t, "change");
    LH_CONTAINS(out, "L3 reason=moved");
    LH_CONTAINS(out, "L4 reason=changed move=0,0 tiles=369 paired=0 ");
    LH_CONTAINS(out, "A4 held\n");
    LH_CONTAINS(out, "L5 reason=moved");
    // A gap, as while a field loads: the 3D has no history and the layer
    // holds with it.
    out = scenario(t, "reset");
    LH_CONTAINS(out, "L4 reason=moved");
    LH_CONTAINS(out, "L5 reason=no-history move=0,0 tiles=369 paired=0 ");
    LH_CONTAINS(out, "A5 held\n");
    LH_CONTAINS(out, "L6 reason=moved");
    auto drawn = redraw(t, "reset");
    LH_CONTAINS(drawn, "Alpha 1: game-frames=6 differ=0 pixels=0");
    LH_EQ(causes(drawn), std::string(" frame-not-blended=100.00%"));
}

LH_TEST(test_tiles_that_do_not_move_together_hold) {
    // Half the tiles move every which way: the layer is not rigid, and
    // everything stays where the newer frame has it.
    auto out = scenario(t, "rigid");
    for (int f = 1; f <= 7; ++f) {
        LH_CONTAINS(out, "L" + std::to_string(f) + " reason=not-rigid");
        LH_CONTAINS(out, "A" + std::to_string(f) + " held\n");
    }
}

LH_TEST(test_the_edge_is_never_uncovered) {
    // The background has no tile to spare at the left edge: where moving it
    // back would uncover the edge, the frame holds; elsewhere it moves.
    auto out = scenario(t, "edge");
    LH_CONTAINS(out, "L3 reason=edge move=-6,0");
    LH_CONTAINS(out, "A3 held\n");
    LH_CONTAINS(out, "L6 reason=edge");
    LH_CONTAINS(out, "L4 reason=moved");
    LH_CHECK_MSG(out.find("held differ") == std::string::npos, out);
    auto drawn = redraw(t, "edge");
    LH_CONTAINS(drawn, "Alpha 1: game-frames=6 differ=0 pixels=0");
    LH_CONTAINS(drawn, "Layer (the main 2D layer of each game frame with 16 tiles or more): frames=7 moved=4 no-history=1 still=0 "
                       "changed=0 not-rigid=0 edge=2 ");
}

