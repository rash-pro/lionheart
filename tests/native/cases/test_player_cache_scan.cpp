// The release scan refuses a player compiled from its own module cache: those
// modules come from a player's discs.
#include "lh_test.h"
#include "../../../tools/player-check/scan.h"

namespace pc = player_check;
namespace {
lh::fs::path database(lh::Case &t, const std::vector<std::string> &arguments, const std::string &source) {
    auto unit = pc::Json::dict().set("directory", t.temp().string()).set("file", source);
    auto args = pc::Json::list();
    args.items.emplace_back("cc");
    for (auto &a : arguments) args.items.emplace_back(a);
    args.items.emplace_back("-c");
    args.items.emplace_back(source);
    unit.set("arguments", std::move(args));
    auto units = pc::Json::list();
    units.items.push_back(std::move(unit));
    auto path = t.temp() / "compile_commands.json";
    lh::write_file(path, units.dump());
    return path;
}
void rejects(const std::function<void()> &run, const std::string &message) {
    try { run(); } catch (const std::runtime_error &e) { LH_CONTAINS(std::string(e.what()), message); return; }
    LH_FAIL("expected rejection: " + message);
}
const std::string cache = "data/lionheart/native/0123456789abcdef0123456789abcdef/fedcba9876543210fedcba9876543210";
}

LH_TEST(test_module_cache_sources_are_rejected) {
    rejects([&] { pc::check_compile_commands(database(t, {}, cache + "/work/1-1/unit_000.c")); }, "generated catalog source");
}

LH_TEST(test_module_cache_includes_are_rejected) {
    rejects([&] { pc::check_compile_commands(database(t, {"-I", cache + "/include"}, "main.c")); }, "generated catalog include");
}

LH_TEST(test_other_folders_named_native_are_allowed) {
    LH_CHECK(pc::check_compile_commands(database(t, {"-Itests/native/support"}, "tests/native/cases/x.c")).at("passed").truth());
}
