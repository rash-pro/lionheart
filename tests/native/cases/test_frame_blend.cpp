// Frame blending on a synthetic double-buffered scene (tests/frame_blend_harness.c):
// two quads that share an edge, each its own transform run, moving and turning;
// a triangle with a corner the GTE never projected, over an edge or touching one
// corner; a cut, a scene change and a gap. The harness records what it draws (runtime/frame_record.h), and the
// offline renderer (tools/devtools/inbetween.c) draws it again.
#include "lh_test.h"

namespace {
namespace fs = lh::fs;

std::string scenario(lh::Case &t, const char *name, const lh::fs::path &record = {}) {
    auto command = t.harness("main").arg(name);
    if (!record.empty()) command.arg(record);
    return t.ok(command.timeout(60)).out;
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
