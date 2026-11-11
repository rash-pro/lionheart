// Frame blending on a synthetic double-buffered scene (tests/frame_blend_harness.c):
// two quads that share an edge, each its own transform run, moving and turning;
// a triangle with a corner the GTE never projected, over an edge or touching one
// corner; a cut, a scene change and a gap; a world moved by the camera on the
// CPU. The harness records what it draws (runtime/frame_record.h), and the
// offline renderer (tools/devtools/inbetween.c) draws it again.
#include "lh_test.h"

namespace {
namespace fs = lh::fs;

std::string scenario(lh::Case &t, const char *name, const lh::fs::path &record = {}) {
    auto command = t.harness("main").arg(name);
    if (!record.empty()) command.arg(record);
    return t.ok(command.timeout(60)).out;
}

std::string straight(lh::Case &t, const char *name) { return t.ok(t.harness("main").arg(name).arg("straight").timeout(60)).out; }

// The smallest and largest edge of the scenario's quads on screen between
// two game frames, in percent of the same edge at the two ends.
std::pair<int, int> size(const std::string &out, int frame) {
    auto low = lh::capture(out, "S" + std::to_string(frame) + R"( resized=\d+ stray=\d+ size=(\d+)-)");
    auto high = lh::capture(out, "S" + std::to_string(frame) + R"( resized=\d+ stray=\d+ size=\d+-(\d+))");
    LH_CHECK_MSG(!low.empty() && !high.empty(), out);
    return {std::stoi(low), std::stoi(high)};
}

std::string redraw(lh::Case &t, const lh::fs::path &record) {
    return t.ok(t.harness("inbetween").arg(record).arg("--causes").timeout(120)).out;
}

// The routes of frame identity (docs/frame-identity.md), each with the window
// recorded: the battles, the prison field walk and the cut-heavy battle.
struct Route {
    const char *id, *script, *card;
    unsigned vblanks;
    const char *window;
};
const Route routes[] = {
    {"magic", "tests/scripts/battle-magic.txt", "disc1-early.mcr", 5400, "2441:3500"},
    {"summon", "tests/scripts/battle-summon.txt", "disc1-early.mcr", 5400, "2880:5150"},
    {"prison", "tests/scripts/play-disc2-prison.txt", "disc2-start.mcr", 32100, "13150:18500"},
    {"camera_cuts", "tests/scripts/fps60-camera-cuts.txt", "disc1-early.mcr", 4400, "2400:4270"},
};

std::string play(lh::Case &t, lh::Command command, const Route &r, const fs::path &dir) {
    for (auto sub : {"states", "shots", "config", "data"}) fs::create_directories(dir / sub);
    lh::write_file(dir / "empty.ini", "");
    fs::copy_file(lh::owned_path(std::string("saves/") + r.card), dir / "card.mcr", fs::copy_options::overwrite_existing);
    command.args({"--config", dir / "empty.ini", "--headless", "--fast", "--mute", "--renderer", "soft", "--stall", "0",
                  "--max-instructions", "0", "--max-vblanks", r.vblanks, "--input-script", t.root() / r.script,
                  "--card1", dir / "card.mcr", "--savestate-dir", dir / "states", "--screenshot-dir", dir / "shots",
                  "--log", fs::path("/dev/null"), "--dump-state", dir / "state.bin", "--native-off"});
    for (int n = 1; n <= 4; ++n) command.arg("--disc").arg(lh::owned_disc(n));
    command.cwd(dir).timeout(7200).env("SDL_VIDEODRIVER", "dummy").env("SDL_AUDIODRIVER", "dummy")
        .env("XDG_CONFIG_HOME", dir / "config").env("XDG_DATA_HOME", dir / "data");
    auto result = t.ok(command);
    LH_CONTAINS(result.out + result.err, "VBlank limit reached");
    return lh::sha256_file(dir / "state.bin");
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
    // To a fraction of a pixel too, for renderers that draw finer: the game's
    // pixels at both ends, less than a pixel from the whole ones between.
    LH_CONTAINS(out, "fractions=fine");
    LH_CHECK_MSG(out.find("fractions=off") == std::string::npos, out);
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

LH_TEST(test_a_world_block_coming_into_view_moves_with_the_world) {
    auto out = scenario(t, "world");
    // A world moved by the camera on the CPU: everything moves, and the quad
    // that comes into view each game frame starts at alpha 0 where the GTE
    // would have put its places in the frame before.
    for (int f = 1; f <= 5; ++f) {
        auto corners = std::to_string(32 + 4 * f), polygons = std::to_string(8 + f);
        LH_CONTAINS(out, "F" + std::to_string(f) + " reason=kept moving=" + corners + "/" + corners + " polygons=" + polygons + "/" + polygons);
        LH_CONTAINS(out, "N" + std::to_string(f) + " mask=f zero=exact");
    }
    auto record = t.temp() / "world.rec";
    scenario(t, "world", record);
    auto drawn = redraw(t, record);
    LH_CONTAINS(drawn, "Alpha 1: game-frames=4 differ=0 pixels=0");
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
    LH_CONTAINS(out, "F3 reason=cut moving=0/8 polygons=0/2 held=0/0/0/8\nT3 turning=0 runs=0/0/0/0\nC3 A=0 B=0\nM3 A=0 B=0");
    LH_CONTAINS(out, "F4 reason=kept moving=8/8");
    LH_CONTAINS(out, "F5 reason=scene moving=0/8");
    LH_CONTAINS(out, "F6 reason=kept moving=8/8");
    LH_CONTAINS(out, "F7 reason=no-history moving=0/8");
    LH_CONTAINS(out, "F8 reason=kept moving=8/8");
}

LH_TEST(test_a_cut_shows_a_game_frame_later) {
    // Drawn as battle draws, from the projections of the frame before: the
    // frame whose projections cut still blends what it draws, and the next
    // one, which draws the cut, holds everything.
    auto out = scenario(t, "late");
    LH_CONTAINS(out, "F3 reason=kept moving=8/8");
    LH_CONTAINS(out, "F4 reason=cut moving=8/8 polygons=2/2");
    LH_CONTAINS(out, "F5 reason=kept moving=0/8 polygons=0/2 held=0/0/0/8");
    LH_CONTAINS(out, "F6 reason=kept moving=8/8");
    auto record = t.temp() / "late.rec";
    scenario(t, "late", record);
    auto drawn = redraw(t, record);
    LH_CONTAINS(drawn, "Alpha 1: game-frames=6 differ=0 pixels=0");
    LH_CONTAINS(drawn, "renders=12 holds=0 skips=0");
    LH_CONTAINS(drawn, "Resets: no-history=1 excluded=0 scene=0 cut=1");
    LH_CONTAINS(drawn, "Stray: corners=0");
    auto causes = lh::capture(drawn, R"(Causes \(share of the pixels that change only at game frames\):([^\n]*))");
    LH_CHECK_MSG(causes.find(" 3d-no-history=") != std::string::npos, drawn);
    for (auto other : {"frame-not-blended=", "3d-held-by-blending="}) LH_CHECK_MSG(causes.find(other) == std::string::npos, drawn);
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
    // And says why: C, which no one frame holds whole, and B's corners held
    // with it; nothing else, and no holes.
    auto causes = lh::capture(drawn, R"(Causes \(share of the pixels that change only at game frames\):([^\n]*))");
    LH_CHECK_MSG(causes.find(" 3d-ambiguous-or-split=") != std::string::npos, drawn);
    LH_CHECK_MSG(causes.find(" 3d-held-by-blending=") != std::string::npos, drawn);
    for (auto other : {"sprite=", "after-display-change=", "3d-unpaired=", "frame-not-blended="})
        LH_CHECK_MSG(causes.find(other) == std::string::npos, drawn);
    LH_CONTAINS(drawn, "Holes (the clear colour where both game frames have 3D): pixels=0 ");
}

LH_TEST(test_a_polygon_touching_one_corner_holds_nothing) {
    // C shares no edge with A, only one position: A and B move whole, as
    // without C. A gap can open only along a shared edge.
    auto out = scenario(t, "touch");
    for (int f = 1; f <= 7; ++f) {
        LH_CONTAINS(out, "F" + std::to_string(f) + " reason=kept moving=8/11 polygons=2/3 held=3/0/0/0");
        LH_CONTAINS(out, "M" + std::to_string(f) + " A=f B=f C=0");
    }
}

LH_TEST(test_a_stretched_surface_is_held_but_not_by_what_is_not_drawn) {
    // Sliding far with C held over B's edge: B would stretch between C's
    // corners and its own, so B is held, and A with it across their edge.
    auto slide = scenario(t, "slide");
    for (int f = 1; f <= 7; ++f) LH_CONTAINS(slide, "M" + std::to_string(f) + " A=0 B=0 C=0");
    // The same with C's last corner clamped far below the screen: the GPU
    // draws none of C, so nothing is held for it, however far A and B move.
    for (auto name : {"slide-hidden", "hidden"}) {
        auto out = scenario(t, name);
        for (int f = 1; f <= 7; ++f) {
            LH_CONTAINS(out, "F" + std::to_string(f) + " reason=kept moving=8/11 polygons=2/3 held=3/0/0/0");
            LH_CONTAINS(out, "M" + std::to_string(f) + " A=f B=f C=0");
        }
    }
}

LH_TEST(test_what_leaves_the_screen_is_drawn_until_it_has_left) {
    // A strip slides left; the game stops drawing a quad once it is wholly
    // off the screen. Between the frame that still drew its last sliver and
    // the next, that quad is drawn beneath the rest where its vertices move,
    // so the screen's edge never shows the background.
    auto out = scenario(t, "pan");
    LH_CONTAINS(out, "L4 leftovers=1\n");
    LH_CONTAINS(out, "L7 leftovers=1\n");
    auto record = t.temp() / "pan.rec";
    scenario(t, "pan", record);
    auto drawn = redraw(t, record);
    LH_CONTAINS(drawn, "Alpha 1: game-frames=6 differ=0 pixels=0");
    LH_CONTAINS(drawn, "Alpha 0: game-frames-equal-to-previous=6");
    LH_CONTAINS(drawn, " culled-at-edge=2 ");
    LH_CONTAINS(drawn, "Holes (the clear colour where both game frames have 3D): pixels=0 ");
}

LH_TEST(test_a_shadow_moves_as_a_unit) {
    // A fan whose centre is projected anew each game frame, as a shadow's
    // is: the centre has no pair, but its ring moves as one, so the centre
    // follows the ring and the whole fan moves.
    auto out = scenario(t, "shadow");
    for (int f = 1; f <= 5; ++f) {
        LH_CONTAINS(out, "F" + std::to_string(f) + " reason=kept moving=24/24 polygons=8/8 held=0/0/0/0");
        LH_CONTAINS(out, "W" + std::to_string(f) + " following=8\n");
    }
    auto record = t.temp() / "shadow.rec";
    scenario(t, "shadow", record);
    auto drawn = redraw(t, record);
    LH_CONTAINS(drawn, "Alpha 1: game-frames=4 differ=0 pixels=0");
    LH_CONTAINS(drawn, "Alpha 0: game-frames-equal-to-previous=4");
    LH_CONTAINS(drawn, "Holes (the clear colour where both game frames have 3D): pixels=0 ");
    // A ring that spreads far as it moves is no unit to follow: held.
    auto torn = scenario(t, "torn");
    for (int f = 1; f <= 5; ++f) {
        LH_CONTAINS(torn, "F" + std::to_string(f) + " reason=kept moving=0/24 polygons=0/8");
        LH_CHECK_MSG(torn.find("W" + std::to_string(f) + " ") == std::string::npos, torn);
    }
}

LH_TEST(test_a_part_that_turns_keeps_its_size) {
    // A square turning a quarter turn each game frame about the line of
    // sight: between the frames it is the same square, turned part of the way.
    auto out = scenario(t, "turn");
    for (int f = 2; f <= 5; ++f) {
        LH_CONTAINS(out, "F" + std::to_string(f) + " reason=kept moving=4/4 polygons=1/1");
        LH_CONTAINS(out, "T" + std::to_string(f) + " turning=4 runs=1/0/0/0");
        LH_CONTAINS(out, "S" + std::to_string(f) + " resized=0 stray=0 ");
        auto [low, high] = size(out, f);
        LH_CHECK_MSG(low >= 95 && high <= 105, out);
    }
    // On straight lines each corner cuts across: the square shrinks.
    auto lines = straight(t, "turn");
    for (int f = 2; f <= 5; ++f) {
        LH_CONTAINS(lines, "S" + std::to_string(f) + " resized=3 ");
        LH_CHECK_MSG(size(lines, f).first < 80, lines);
    }
    // Drawn again from a recording: alpha 1 is the game's picture.
    auto record = t.temp() / "turn.rec";
    scenario(t, "turn", record);
    auto drawn = redraw(t, record);
    LH_CONTAINS(drawn, "Alpha 1: game-frames=");
    LH_CONTAINS(drawn, " differ=0 pixels=0");
    LH_CONTAINS(drawn, "Stray: corners=0 ");
    LH_CONTAINS(drawn, " resized=0 (0.00%)");
}

LH_TEST(test_a_joint_between_two_parts_stays_welded) {
    // B hangs from A's far end and bends against it: B turns relative to A,
    // its origin carried by A, so the joint both draw stays one point and
    // neither rod changes length.
    auto out = scenario(t, "chain");
    for (int f = 2; f <= 5; ++f) {
        LH_CONTAINS(out, "F" + std::to_string(f) + " reason=kept moving=8/8 polygons=2/2");
        LH_CONTAINS(out, "T" + std::to_string(f) + " turning=8 runs=2/1/0/0");
        LH_CONTAINS(out, "E" + std::to_string(f) + " ends=exact shared=kept");
        LH_CONTAINS(out, "S" + std::to_string(f) + " resized=0 stray=0 ");
        auto [low, high] = size(out, f);
        LH_CHECK_MSG(low >= 92 && high <= 108, out);
    }
    auto lines = straight(t, "chain");
    for (int f = 2; f <= 5; ++f) LH_CHECK_MSG(lines.find("S" + std::to_string(f) + " resized=0 ") == std::string::npos, lines);
}

LH_TEST(test_a_turn_near_half_a_turn_moves_on_straight_lines) {
    // 175 degrees in one game frame: either way round is about as likely,
    // so nothing turns; the corners keep to straight lines between the ends.
    auto out = scenario(t, "far");
    for (int f = 2; f <= 5; ++f) {
        LH_CONTAINS(out, "F" + std::to_string(f) + " reason=kept moving=4/4 polygons=1/1");
        LH_CONTAINS(out, "T" + std::to_string(f) + " turning=0 runs=0/0/0/1");
        LH_CONTAINS(out, "S" + std::to_string(f) + " resized=3 stray=0 ");
    }
}

LH_TEST(test_a_matrix_that_is_no_rotation) {
    // Flattened onto a plane (a shadow): no rotation to follow.
    auto flat = scenario(t, "flat");
    for (int f = 1; f <= 5; ++f) LH_CONTAINS(flat, "T" + std::to_string(f) + " turning=0 runs=0/0/1/0");
    // Mirrored at one end only: no turn between them.
    LH_CONTAINS(scenario(t, "mirror"), "T1 turning=0 runs=0/0/1/0");
    // Mirrored at both ends, or scaled unevenly: the rotation turns and the
    // rest is kept, so the part keeps its size.
    for (auto name : {"mirrored", "stretched"}) {
        auto out = scenario(t, name);
        for (int f = 2; f <= 5; ++f) {
            LH_CONTAINS(out, "T" + std::to_string(f) + " turning=4 runs=1/0/0/0");
            LH_CONTAINS(out, "S" + std::to_string(f) + " resized=0 stray=0 ");
        }
    }
}

LH_TEST(test_corners_that_meet_on_screen_are_told_apart_by_the_frame_before) {
    // Two vertices of one part meet on one pixel for one game frame, having
    // stood apart before: the service holds both corners there as ambiguous.
    // The same polygons a game frame earlier say which vertex each corner is.
    auto out = scenario(t, "cross");
    LH_CONTAINS(out, "F3 reason=kept moving=7/7 polygons=2/2 held=0/0/0/0\nT3 turning=7 runs=1/0/0/0\nR3 resolved=2\n");
    for (int f : {1, 2, 4, 5}) {
        LH_CONTAINS(out, "F" + std::to_string(f) + " reason=kept moving=7/7 polygons=2/2");
        LH_CHECK_MSG(out.find("R" + std::to_string(f) + " ") == std::string::npos, out);
    }
}

LH_TEST(test_colours_blend_between_game_frames) {
    // A, shaded per corner, fades up; B, flat, fades down: halfway each
    // changing colour is halfway, and the ends are the game's. C is textured
    // and its semi-transparency changes with its colour: drawn as recorded.
    auto out = scenario(t, "fade");
    LH_CONTAINS(out, "C2 A=b/000028-00003c-000050 B=f/00a000-008c00-007800 C=0\n");
    LH_CONTAINS(out, "C3 A=b/000050-000064-000078 B=f/007800-006400-005000 C=0\n");
    LH_CONTAINS(out, "C5 A=b/0000a0-0000b4-0000c8 B=f/002800-001400-000000 C=0\n");
    // A frame without history has nothing to blend from.
    LH_CONTAINS(out, "C1 A=0 B=0 C=0\n");
    // Polygons whose colour stays are drawn as recorded.
    LH_CONTAINS(scenario(t, "move"), "C3 A=0 B=0\n");
    auto record = t.temp() / "fade.rec";
    scenario(t, "fade", record);
    auto drawn = redraw(t, record);
    LH_CONTAINS(drawn, " differ=0 pixels=0");
    LH_CONTAINS(drawn, "Stray: corners=0 ");
}

// Recording changes nothing the game computes; replaying the recording gives
// every picture the game showed, and every game frame drawn again with its
// polygons at the end of their interval is that picture, pixel for pixel.
LH_GAME_TEST_P(test_routes_draw_again_exactly, routes) {
    const Route &r = p;
    auto record = t.temp() / "on" / "frames.rec";
    auto off = play(t, t.player(), r, t.temp() / "off");
    auto on = play(t, t.player().args({"--frame-record", record, "--frame-record-window", r.window}), r, t.temp() / "on");
    LH_EQ(on, off);
    auto result = t.run(t.harness("inbetween").arg(record).timeout(3600));
    lh::write_file(t.temp() / "inbetween.txt", result.out + result.err);
    LH_CHECK_MSG(result.rc == 0, "the replay differs:\n" + lh::excerpt(result.out + result.err));
    LH_CONTAINS(result.out, "Alpha 1: game-frames=");
    LH_CHECK_MSG(lh::capture(result.out, R"(Alpha 1: game-frames=(\d+))") != "0", result.out);
    LH_CONTAINS(result.out, "Stray: corners=0 ");
    if (std::string(r.id) == "camera_cuts") {
        // The route cuts the camera to every attacker and back; frame
        // identity's screen measure finds 17 jumps, 12 of them cuts. The
        // other five are fast moves that follow a cut, and blend.
        auto held = std::stoi(lh::capture(result.out, R"(Jumps \(median paired screen move of 32 pixels or more\): held=(\d+))"));
        LH_CHECK_MSG(held >= 12, result.out);
    }
}
