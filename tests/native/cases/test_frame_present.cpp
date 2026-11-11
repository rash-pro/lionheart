// The live presenter (runtime/frame_present.h, docs/frame-present.md): its
// phases on synthetic intervals, and on the synthetic scenes of frame blending
// and frame layers (tests/frame_blend_harness.c, tests/frame_layer_harness.c)
// the same picture at every VBlank as the offline renderer
// (tools/devtools/inbetween.c) draws from the same recording.
#include "lh_test.h"
#include <map>
#include <sstream>

namespace {
namespace fs = lh::fs;

// Records a scene, draws it offline and live, and compares the two at every
// VBlank. Returns the live harness's summary.
std::string both(lh::Case &t, const char *harness, const char *scene, bool fractions = false) {
    auto record = t.temp() / (std::string(scene) + ".rec"), offline = t.temp() / scene;
    fs::create_directories(offline);
    t.ok(t.harness(harness).arg(scene).arg(record).timeout(60));
    t.ok(t.harness("inbetween").arg(record).arg("--out").arg(offline).timeout(120));
    auto live = t.temp() / (std::string(scene) + "-live.rgb");
    auto command = t.harness("main").arg("replay").arg(record).arg(live);
    if (fractions) command.arg("fractions");
    auto out = t.ok(command.timeout(120)).out;
    auto a = lh::read_file(offline / "on.rgb"), b = lh::read_file(live);
    LH_CHECK_MSG(!a.empty() && a.size() % (320 * 240 * 3) == 0, scene);
    LH_EQ(a.size(), b.size());
    for (size_t frame = 0; frame * 320 * 240 * 3 < a.size(); ++frame)
        LH_CHECK_MSG(a.compare(frame * 320 * 240 * 3, 320 * 240 * 3, b, frame * 320 * 240 * 3, 320 * 240 * 3) == 0,
                     std::string(scene) + ": VBlank picture " + std::to_string(frame) + " differs");
    LH_CONTAINS(out, "failures=0");
    return out;
}

// Each scenario's fractions, as the harness prints them.
std::string phases(const std::string &out, const std::string &name) {
    std::istringstream in(out);
    for (std::string line; std::getline(in, line);) if (line.rfind(name + ":", 0) == 0) return line;
    LH_CHECK_MSG(false, "no " + name + " in " + out);
    return {};
}
} // namespace

LH_TEST(test_phases_follow_the_measured_interval) {
    auto out = t.ok(t.harness("main").arg("phases")).out;
    // A game frame that ended n VBlanks after the one before is drawn at k / n
    // the k-th VBlank after its end, whole from the n-th until the next ends;
    // the first has no interval to go by.
    LH_EQ(phases(out, "two"), "two: 11=whole 12=whole 13=0.50 14=whole 15=0.50 16=whole 17=0.50 18=whole "
                              "between=3 holds=0 skips=0");
    LH_EQ(phases(out, "three"), "three: 11=whole 12=whole 13=whole 14=0.33 15=0.67 16=whole 17=0.33 18=0.67 19=whole "
                                "20=0.33 21=0.67 22=whole between=6 holds=0 skips=0");
    LH_EQ(phases(out, "four"), "four: 11=whole 12=whole 13=whole 14=whole 15=0.25 16=0.50 17=0.75 18=whole 19=0.25 "
                               "20=0.50 21=0.75 22=whole 23=0.25 24=0.50 25=0.75 26=whole between=9 holds=0 skips=0");
    LH_CONTAINS(phases(out, "six"), "17=0.17 18=0.33 19=0.50 20=0.67 21=0.83 22=whole 23=0.17");
    LH_CONTAINS(phases(out, "six"), "between=15 holds=0 skips=0");
    // Faster: the frame that comes early cuts the one before short (one skip),
    // and goes by its own, shorter interval at once.
    LH_EQ(phases(out, "faster"), "faster: 11=whole 12=whole 13=whole 14=whole 15=0.25 16=0.50 17=0.75 18=whole 19=0.25 "
                                 "20=0.50 21=0.50 22=whole 23=0.50 24=whole between=7 holds=0 skips=1");
    // Slower: the frame stays whole until the next comes (holds).
    LH_EQ(phases(out, "slower"), "slower: 11=whole 12=whole 13=0.50 14=whole 15=0.50 16=whole 17=whole 18=whole "
                                 "19=0.25 20=0.50 21=0.75 22=whole 23=0.25 24=0.50 25=0.75 26=whole "
                                 "between=8 holds=2 skips=0");
    // After a gap of more than eight VBlanks there is nothing to go by; a frame
    // that must be shown whole (an excluded scene) is.
    LH_CONTAINS(phases(out, "gap"), "30=whole 31=whole 32=whole 33=0.50 34=whole");
    LH_CONTAINS(phases(out, "whole"), "18=whole 19=whole 20=whole 21=whole 22=whole 23=0.25");
    // Between VBlanks the fraction follows the moment.
    LH_CONTAINS(out, "moment: 15=256/1024 15.5=384/1024");
}

LH_TEST(test_waiting_for_the_first_frame_draws_only_what_is_new) {
    // Before a game's first frame ends (the screens it starts with) each
    // picture draws what the guest sent since the one before, however long
    // that lasts, and shows what the guest shows.
    auto out = t.ok(t.harness("main").arg("waiting")).out;
    LH_CONTAINS(out, "waiting: first=3 last=3 same=1 failures=0");
}

LH_TEST(test_live_and_offline_draw_the_same_pictures) {
    // Two parts moving and turning, a cut, a change of scene and a gap.
    for (auto scene : {"move", "resets", "turn", "chain", "shadow", "pan"}) {
        auto out = both(t, "scene", scene);
        if (std::string(scene) == "move") LH_CHECK_MSG(lh::capture(out, R"(blended=(\d+))") != "0", out);
    }
}

LH_TEST(test_live_and_offline_draw_the_same_field_layers) {
    // A scrolling field, an animated tile, parallax, a field change and a gap.
    for (auto scene : {"scroll", "animated", "parallax", "change", "reset"}) both(t, "layers", scene);
}

LH_TEST(test_fractions_leave_the_whole_pixels_alone) {
    // Fractions of a pixel go to a renderer that draws finer; the PlayStation's
    // own pixels, which the software rasterizer draws, are the same.
    both(t, "scene", "move", true);
    both(t, "layers", "scroll", true);
}

LH_TEST(test_off_binds_nothing) {
    // Until smooth motion starts, and once it stops, the plain GTE entries are
    // bound and nothing observes the guest's GPU.
    auto out = t.ok(t.harness("host")).out;
    LH_CONTAINS(out, "before: plain binds=0");
    LH_CONTAINS(out, "running: recording entries, own GPU");
    LH_CONTAINS(out, "after: plain binds=2");
}

namespace {
// One scenario's numbers, as the pacing harness prints them.
struct Paced {
    double on_time = 0, uneven = 100;
    std::map<int, double> intervals;
    std::string multiples, line;
    long changes = -1;
};
Paced paced(const std::string &out, const std::string &name) {
    Paced p;
    p.line = phases(out, name);
    p.on_time = std::stod(lh::capture(p.line, R"(on_time=([0-9.]+)%)"));
    p.uneven = std::stod(lh::capture(p.line, R"(uneven=([0-9.]+)%)"));
    p.multiples = lh::capture(p.line, R"(multiples=(\S*))");
    p.changes = std::stol(lh::capture(p.line, R"(changes=(\d+))"));
    std::istringstream in(lh::capture(p.line, R"(intervals=(\S*))"));
    for (std::string item; std::getline(in, item, ',');)
        if (!item.empty()) p.intervals[std::stoi(item)] = std::stod(item.substr(item.find(':') + 1));
    return p;
}
} // namespace

LH_TEST(test_pictures_come_on_the_refresh_they_are_drawn_for) {
    auto out = t.ok(t.harness("pace")).out;
    // Fixed grids at 60, 120 and 144 Hz whose compositor deadline drifts by
    // most of a refresh, with reports off the grid, and a virtual output whose
    // period is not said and is a little off: a picture every refresh, on the
    // refresh it was drawn for, the game evenly spaced.
    for (auto name : {"60", "120", "144", "virtual"}) {
        auto p = paced(out, name);
        LH_CHECK_MSG(p.on_time >= 99.5, p.line);
        LH_CHECK_MSG(p.intervals[1] >= 99.5, p.line);
        LH_CHECK_MSG(p.multiples == "1:" + lh::capture(p.line, R"(pictures=(\d+))") + ",", p.line);
        LH_CHECK_MSG(p.uneven <= 1, p.line);
    }
    // A picture that does not fit a refresh: every second; one that does not
    // fit two: every few, with copies of it between.
    auto battle = paced(out, "144-battle");
    LH_CHECK_MSG(battle.on_time >= 99.5 && battle.intervals[2] >= 99.5, battle.line);
    auto heavy = paced(out, "144-heavy");
    LH_CHECK_MSG(heavy.on_time >= 99.5, heavy.line);
    LH_CHECK_MSG(heavy.intervals.rbegin()->first >= 3 && (heavy.intervals[3] >= 99.5 || heavy.intervals[4] >= 99.5), heavy.line);
    // From every refresh to every second and back: once each way, every
    // interval one refresh or two, and the game evenly spaced through both.
    auto change = paced(out, "144-change");
    LH_CHECK_MSG(change.on_time >= 99.5 && change.changes == 2, change.line);
    LH_CHECK_MSG(change.intervals[1] + change.intervals[2] >= 99.9 && change.intervals[1] > 50 && change.intervals[2] > 10, change.line);
    LH_CHECK_MSG(change.uneven <= 0.5, change.line);
    // Without targets, handed over in the refresh before the one before.
    auto untargeted = paced(out, "untargeted");
    LH_CHECK_MSG(untargeted.on_time >= 99.5 && untargeted.intervals[1] >= 99.5, untargeted.line);
}
