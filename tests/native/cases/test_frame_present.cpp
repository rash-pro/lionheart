// The live presenter (runtime/frame_present.h, docs/frame-present.md): its
// phases on synthetic intervals, and on the synthetic scenes of frame blending
// and frame layers (tests/frame_blend_harness.c, tests/frame_layer_harness.c)
// the same picture at every VBlank as the offline renderer
// (tools/devtools/inbetween.c) draws from the same recording.
#include "lh_test.h"
#include <algorithm>
#include <cmath>
#include <map>
#include <sstream>
#include <vector>

namespace {
namespace fs = lh::fs;

// Records a scene, draws it offline and live, and compares the two at every
// VBlank. Returns the live harness's summary.
std::string both(lh::Case &t, const char *harness, const char *scene, bool fractions = false, const char *precision = nullptr) {
    auto record = t.temp() / (std::string(scene) + ".rec"), offline = t.temp() / scene;
    fs::create_directories(offline);
    t.ok(t.harness(harness).arg(scene).arg(record).timeout(60));
    t.ok(t.harness("inbetween").arg(record).arg("--out").arg(offline).timeout(120));
    auto live = t.temp() / (std::string(scene) + "-live.rgb");
    auto command = t.harness("main").arg("replay").arg(record).arg(live);
    if (fractions) command.arg("fractions");
    if (precision) command.arg(precision);
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
    // that lasts, and shows what the guest showed at its last VBlank: not
    // the next picture half drawn.
    auto out = t.ok(t.harness("main").arg("waiting")).out;
    LH_CONTAINS(out, "waiting: first=3 last=3 same=1 held=1 failures=0");
}

LH_TEST(test_live_and_offline_draw_the_same_pictures) {
    // Two parts moving and turning, a cut, a change of scene and a gap.
    for (auto scene : {"move", "resets", "turn", "chain", "shadow", "pan", "pan-shaded"}) {
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

namespace {
// What the presentation GPU hands its rasterizer with PGXP's precision on
// (the probe harness): per picture, each triangle's corners and colour.
struct Corner {
    double x = 0, y = 0, w = 0;
};
struct Triangle {
    Corner c[3];
    std::string colour;
};
std::vector<std::vector<Triangle>> drawn(lh::Case &t, const char *scene, const char *precision, bool split = false) {
    auto record = t.temp() / (std::string(scene) + "-" + precision + ".rec");
    t.ok(t.harness("scene").arg(scene).arg(record).timeout(60));
    auto command = t.harness("probe").arg("replay").arg(record).arg(t.temp() / "probe.rgb").arg("fractions").arg(precision);
    if (split) command.arg("split");
    auto out = t.ok(command.timeout(120)).out;
    std::vector<std::vector<Triangle>> pictures;
    std::istringstream in(out);
    for (std::string line; std::getline(in, line);) {
        if (line.rfind("V", 0) == 0) pictures.emplace_back();
        else if (line.rfind("T ", 0) == 0 && !pictures.empty()) {
            std::istringstream words(line.substr(2));
            Triangle tri;
            for (auto &c : tri.c) words >> c.x >> c.y >> c.w;
            words >> tri.colour;
            pictures.back().push_back(tri);
        }
    }
    LH_CHECK_MSG(pictures.size() > 12, out);
    return pictures;
}
bool whole(double v) { return v == static_cast<double>(static_cast<long>(v)); }
} // namespace

LH_TEST(test_precision_leaves_the_whole_pixels_alone) {
    // PGXP's precision goes to a renderer that draws finer; the PlayStation's
    // own pixels are the same, live and offline.
    both(t, "scene", "move", true, "perspective");
    both(t, "scene", "held", true, "vertex");
    both(t, "layers", "scroll", true, "perspective");
}

LH_TEST(test_precision_keeps_a_still_part_still_and_fine) {
    // Two rods standing turned: once the frames have a history, every
    // picture, game frames and those between, draws each corner at the same
    // place, finer than a pixel, with its depth.
    // The scene draws into two buffers 256 lines apart, in either order.
    auto pictures = drawn(t, "still", "perspective");
    auto corners = [](const std::vector<Triangle> &picture) {
        std::vector<std::string> out;
        for (const auto &tri : picture) {
            std::ostringstream s;
            for (const auto &c : tri.c) s << c.x << "," << std::fmod(c.y, 256.0) << "," << c.w << " ";
            out.push_back(s.str());
        }
        std::sort(out.begin(), out.end());
        return out;
    };
    auto last = corners(pictures.back());
    bool fine = false;
    LH_CHECK_MSG(last.size() == 4, "triangles: " + std::to_string(last.size()));
    for (size_t p = pictures.size() - 12; p < pictures.size(); ++p) {
        LH_CHECK_MSG(corners(pictures[p]) == last, "picture " + std::to_string(p) + " moved a corner");
        for (const auto &tri : pictures[p])
            for (const auto &c : tri.c) {
                LH_CHECK_MSG(c.w > 0 && c.w < 1000, "picture " + std::to_string(p) + " has no projected depth");
                if (!whole(c.x) || !whole(c.y)) fine = true;
            }
    }
    LH_CHECK_MSG(fine, "every corner at a whole pixel");
}

LH_TEST(test_precision_takes_the_guests_pgxp_where_blending_does_not) {
    // A triangle over B's right edge with a corner the GTE never projected:
    // its corners are held, so they come from the guest's PGXP (the stand-in
    // puts them a quarter right and an eighth up), and the one PGXP does not
    // know stays whole. A polygon with a corner of no depth is drawn without
    // perspective; the quads, every corner known, with it.
    auto pictures = drawn(t, "held", "perspective");
    size_t held = 0, quads = 0;
    for (size_t p = 4; p < pictures.size(); ++p)
        for (const auto &tri : pictures[p]) {
            if (tri.colour == "0000c0") {
                ++held;
                for (const auto &c : tri.c) {
                    LH_EQ(c.w, 0.0);
                    if (c.x == 300) LH_CHECK_MSG(whole(c.y), "the corner PGXP does not know moved");
                    else LH_CHECK_MSG(c.x - std::floor(c.x) == 0.25 && std::ceil(c.y) - c.y == 0.125,
                                      "held corner at " + std::to_string(c.x) + "," + std::to_string(c.y));
                }
            } else {
                ++quads;
                for (const auto &c : tri.c) LH_CHECK_MSG(c.w > 0, "a quad without depth");
            }
        }
    LH_CHECK_MSG(held > 0 && quads > 0, "nothing drawn");
    // In vertex mode nothing carries a depth.
    for (const auto &picture : drawn(t, "held", "vertex"))
        for (const auto &tri : picture)
            for (const auto &c : tri.c) LH_EQ(c.w, 0.0);
}

LH_TEST(test_precision_reaches_polygons_culled_at_the_edge) {
    // The strip's recovered quads use the same projection as their visible
    // neighbours, including depth. A whole-pixel affine quad would break
    // that surface just before it leaves the screen.
    auto pictures = drawn(t, "pan-shaded", "perspective");
    size_t corners = 0;
    for (size_t p = 8; p < pictures.size(); ++p)
        for (const auto &tri : pictures[p]) {
            if (tri.colour != "808080") continue;
            for (const auto &c : tri.c) {
                ++corners;
                LH_EQ(c.w, 1200.0);
                LH_CHECK_MSG(!whole(c.x) || !whole(c.y), "a culled corner lost its fraction");
            }
        }
    LH_CHECK_MSG(corners > 0, "no strip corners drawn");
}

LH_TEST(test_precision_survives_split_packets) {
    // Splitting every GPU write must preserve endpoint precision even
    // though split packets are held rather than interpolated.
    for (auto mode : {"vertex", "perspective"}) {
        auto whole_packets = drawn(t, "still", mode), split_packets = drawn(t, "still", mode, true);
        LH_EQ(whole_packets.size(), split_packets.size());
        for (size_t p = 8; p < whole_packets.size(); ++p) {
            LH_EQ(whole_packets[p].size(), split_packets[p].size());
            for (size_t i = 0; i < whole_packets[p].size(); ++i)
                for (size_t c = 0; c < 3; ++c) {
                    LH_EQ(whole_packets[p][i].c[c].x, split_packets[p][i].c[c].x);
                    LH_EQ(whole_packets[p][i].c[c].y, split_packets[p][i].c[c].y);
                    LH_EQ(whole_packets[p][i].c[c].w, split_packets[p][i].c[c].w);
                }
        }
    }
}
