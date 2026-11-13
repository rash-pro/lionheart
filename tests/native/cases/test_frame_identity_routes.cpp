// Frame identity on the owner's discs: the default player with the service off
// and on, and a developer build with the provenance oracle (FF8_FRAME_ORACLE)
// that judges every pair and corner link the service makes. The bars are those
// set for battle and field 3D before stage 1 was built:
//   at least 90% of the corners of drawn 3D polygons linked (interpolable),
//   wrong pairs at most 0.01% of the pairs the oracle decides,
//   wrong corner links at most 0.1% of the links it decides,
// and, on the routes that cross the world map, the world map's own check
// (check_world), which it had to pass before it was published.
#include "lh_test.h"
#include <cstdlib>
#include <sstream>

namespace {
namespace fs = lh::fs;

struct Route {
    const char *id, *script, *card;
    unsigned vblanks;
    const char *scenes; // scenes held to the bars
    bool world;         // the world map's own check
};
const Route routes[] = {
    {"magic", "tests/scripts/battle-magic.txt", "disc1-early.mcr", 5400, "battle", true},
    {"summon", "tests/scripts/battle-summon.txt", "disc1-early.mcr", 5400, "battle", false},
    {"prison", "tests/scripts/play-disc2-prison.txt", "disc2-start.mcr", 32100, "field battle", false},
    {"identical_enemies", "tests/scripts/fps60-identical-enemies.txt", "disc2-start.mcr", 23000, "battle", false},
    {"camera_cuts", "tests/scripts/fps60-camera-cuts.txt", "disc1-early.mcr", 4400, "battle", false},
    {"world_walk", "tests/scripts/fps60-world-walk.txt", "disc1-early.mcr", 3000, "", true},
    {"world_town", "tests/scripts/fps60-world-town.txt", "disc1-early.mcr", 3200, "", true},
    {"world_car", "tests/scripts/fps60-world-car.txt", "disc3-start.mcr", 7600, "", true},
};

using Values = std::map<std::string, double>;

// key=value numbers of the first line starting with prefix; empty if none.
Values fields(const std::string &text, const std::string &prefix) {
    Values out;
    std::istringstream lines(text);
    std::string line;
    while (std::getline(lines, line)) {
        if (line.rfind(prefix, 0) != 0) continue;
        std::istringstream words(line.substr(prefix.size()));
        std::string word;
        while (words >> word) {
            auto eq = word.find('=');
            if (eq == std::string::npos) continue;
            out[word.substr(0, eq)] = std::strtod(word.c_str() + eq + 1, nullptr);
        }
        break;
    }
    return out;
}

fs::path env_path(const char *name, const fs::path &fallback) {
    const char *value = std::getenv(name);
    return value && *value ? fs::path(value) : fallback;
}

struct Run {
    std::string out, state;
};

Run play(lh::Case &t, lh::Command command, const Route &r, const fs::path &dir) {
    fs::create_directories(dir / "states");
    fs::create_directories(dir / "shots");
    fs::create_directories(dir / "config");
    fs::create_directories(dir / "data");
    lh::write_file(dir / "empty.ini", "");
    fs::copy_file(lh::owned_path(std::string("saves/") + r.card), dir / "card.mcr", fs::copy_options::overwrite_existing);
    command.args({"--config", dir / "empty.ini", "--headless", "--fast", "--mute", "--renderer", "soft", "--stall", "0",
                  "--max-instructions", "0", "--max-vblanks", r.vblanks, "--input-script", t.root() / r.script,
                  "--card1", dir / "card.mcr", "--savestate-dir", dir / "states", "--screenshot-dir", dir / "shots",
                  "--log", fs::path("/dev/null"), "--dump-state", dir / "state.bin"});
    for (int n = 1; n <= 4; ++n) command.arg("--disc").arg(lh::owned_disc(n));
    command.cwd(dir).timeout(7200).env("SDL_VIDEODRIVER", "dummy").env("SDL_AUDIODRIVER", "dummy")
        .env("XDG_CONFIG_HOME", dir / "config").env("XDG_DATA_HOME", dir / "data");
    auto result = t.ok(command);
    lh::write_file(dir / "run.log", result.out + result.err);
    LH_CONTAINS(result.out + result.err, "VBlank limit reached");
    return {result.out, lh::sha256_file(dir / "state.bin")};
}

std::string digest(const std::string &out) { return lh::capture(out, R"(Frame identity digest: ([0-9a-f]{16}))"); }

void check_scene(const std::string &route, const std::string &scene, const std::string &service, const std::string &oracle) {
    auto s = fields(service, "Frame identity " + scene + ": "), o = fields(oracle, "T " + scene + " ");
    const std::string where = route + " " + scene + ": ";
    LH_CHECK_MSG(!s.empty() && !o.empty() && s["frames"] > 0, where + "no frames of this scene");
    double decided = o["correct"] + o["wrong"] + o["equivalent"];
    double links = o["link-correct"] + o["link-equivalent"] + o["link-wrong"];
    LH_CHECK_MSG(decided >= 0.3 * o["matched"], where + "the oracle decides too few pairs to judge");
    LH_CHECK_MSG(s["linked"] >= 0.90 * s["corners3d"],
                 where + "linked " + std::to_string(s["linked"]) + " of " + std::to_string(s["corners3d"]) + " 3D corners");
    LH_CHECK_MSG(o["wrong"] <= 0.0001 * decided,
                 where + "wrong pairs " + std::to_string(o["wrong"]) + " of " + std::to_string(decided) + " decided");
    LH_CHECK_MSG(o["link-wrong"] <= 0.001 * links,
                 where + "wrong links " + std::to_string(o["link-wrong"]) + " of " + std::to_string(links) + " decided");
}
// The world map's own check. Its terrain is moved by the camera on the CPU and
// paired by a voted step; the game's record of where its camera stood says
// how far every such point went. Every frame's step must be that change and
// every such pair one place in the world. Pairs are wrong where the oracle's
// provenance says another source and the camera another place, or where
// provenance cannot decide and the camera says another place: at most 0.1% of
// the decided. Another record of the same place (an edge both blocks keep) is
// harmless. Points with no record in the frame before (blocks coming into
// view) are placed at their input less the step: right only where the step is
// the camera's change. Linked corners whose pair neither can decide must stay
// rare, and at least 90% of the corners of drawn 3D polygons must be linked.
void check_world(const std::string &route, const std::string &service, const std::string &oracle) {
    auto s = fields(service, "Frame identity world points: "), o = fields(oracle, "T world ");
    const std::string where = route + " world: ";
    LH_CHECK_MSG(!s.empty() && !o.empty() && s["step-same"] > 0, where + "no world frames with a step");
    LH_CHECK_MSG(s["step-other"] == 0, where + std::to_string(s["step-other"]) + " frames whose step is not the camera's");
    LH_CHECK_MSG(s["place-other"] == 0, where + std::to_string(s["place-other"]) + " pairs at two places");
    LH_CHECK_MSG(o["placed-wrong"] == 0, where + std::to_string(o["placed-wrong"]) + " points placed by a wrong step");
    auto w = fields(service, "Frame identity world: ");
    LH_CHECK_MSG(w["excluded"] == 0 && w["paired"] > 0, where + "the world map is not published");
    LH_CHECK_MSG(w["linked"] >= 0.90 * w["corners3d"],
                 where + "linked " + std::to_string(w["linked"]) + " of " + std::to_string(w["corners3d"]) + " 3D corners");
    double provenance = o["correct"] + o["wrong"] + o["equivalent"];
    double decided = provenance + o["place-correct"] + o["place-wrong"];
    double wrong = o["wrong"] - o["wrong-same-place"] + o["place-wrong"];
    LH_CHECK_MSG(provenance >= 0.3 * o["matched"], where + "the oracle decides too few pairs to judge");
    LH_CHECK_MSG(wrong <= 0.001 * decided,
                 where + "wrong pairs " + std::to_string(wrong) + " of " + std::to_string(decided) + " decided");
    LH_CHECK_MSG(o["corner-pair-wrong"] <= 0.001 * o["linked"],
                 where + "linked corners on wrong pairs " + std::to_string(o["corner-pair-wrong"]));
    LH_CHECK_MSG(o["corner-pair-undecided"] <= 0.01 * o["linked"],
                 where + "linked corners on undecided pairs " + std::to_string(o["corner-pair-undecided"]));
    LH_CHECK_MSG(o["link-wrong"] <= 0.001 * (o["link-correct"] + o["link-equivalent"] + o["link-wrong"]),
                 where + "wrong links " + std::to_string(o["link-wrong"]));
}
} // namespace

LH_GAME_TEST_P(test_routes_meet_the_identity_bars, routes) {
    const Route &r = p;
    auto oracle_host = env_path("LIONHEART_FRAME_ORACLE", t.root() / "build/frame-oracle/lionheart");
    LH_CHECK_MSG(fs::is_regular_file(oracle_host),
                 "build the oracle host: cmake -S . -B build/frame-oracle -DFF8_FRAME_ORACLE=ON && "
                 "cmake --build build/frame-oracle --target ff8, or set LIONHEART_FRAME_ORACLE");
    auto off = play(t, t.player().arg("--native-off"), r, t.temp() / "off");
    auto on = play(t, t.player().args({"--native-off", "--frame-identity"}), r, t.temp() / "on");
    auto report = t.temp() / "oracle" / "report.txt";
    fs::create_directories(report.parent_path());
    auto judged = play(t, t.target(oracle_host).args({"--interpreter", "--frame-oracle", report}), r, t.temp() / "oracle");
    // Observing changes nothing the game computes, and both builds see one stream.
    LH_EQ(on.state, off.state);
    LH_EQ(judged.state, off.state);
    LH_EQ(digest(judged.out), digest(on.out));
    auto text = lh::read_file(report);
    LH_CHECK_MSG(text.find("LOST") == std::string::npos, "the oracle lost track:\n" + lh::excerpt(text));
    std::istringstream scenes(r.scenes);
    std::string scene;
    while (scenes >> scene) check_scene(r.id, scene, on.out, text);
    if (r.world) check_world(r.id, on.out, text);
}

// Native modules see the same GTE calls with the same values as the
// interpreter, so the stream must not change by a byte.
LH_GAME_TEST(test_native_modules_give_the_interpreters_stream) {
    auto host = env_path("LIONHEART_NATIVE_HOST", t.root() / "build/dev-modules/ff8");
    auto modules = env_path("LIONHEART_NATIVE_MODULES", t.root() / "build/modules/modules");
    auto exe = env_path("LIONHEART_NATIVE_EXE", t.root() / "build/generated-modules/extracted/SLUS_008.92");
    LH_CHECK_MSG(fs::is_regular_file(host) && fs::is_directory(modules) && fs::is_regular_file(exe),
                 "build a developer host and its modules as docs/native-modules.md describes "
                 "(build/dev-modules, build/modules), or set LIONHEART_NATIVE_HOST, LIONHEART_NATIVE_MODULES and LIONHEART_NATIVE_EXE");
    const Route &r = routes[0];
    auto interpreted = play(t, t.player().args({"--native-off", "--frame-identity"}), r, t.temp() / "interpreted");
    auto native = play(t, t.target(host).args({exe, "--native-modules", modules, "--frame-identity"}), r, t.temp() / "native");
    LH_CONTAINS(lh::read_file(t.temp() / "native" / "run.log"), "Native modules: ");
    LH_CHECK_MSG(lh::capture(lh::read_file(t.temp() / "native" / "run.log"), R"(Native modules: (\d+) registered)") != "0",
                 "no module was registered");
    LH_EQ(digest(native.out), digest(interpreted.out));
    LH_EQ(native.state, interpreted.state);
}
