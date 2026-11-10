// The player's background build of native code, with synthetic sets and a fake
// compiler: what reaches the cache and when it counts, the order sets are built
// in, failures, and two players sharing one data folder.
#include "lh_test.h"
#include "lh_internal.h"

#include <algorithm>
#include <chrono>
#include <sstream>
#include <thread>

namespace {
struct Build {
    lh::Case &t;
    lh::fs::path root, log, spec;
    explicit Build(lh::Case &c) : t(c), root(c.temp() / "native"), log(c.temp() / "cc.log"), spec(c.temp() / "spec.txt") {}
    void sets(const std::string &text) { lh::write_file(spec, text); }
    lh::Command command(std::initializer_list<lh::Arg> extra = {}) const {
        auto command = t.harness("native_build");
        command.args({"--root", root, "--compiler", lh::fs::path(t.harness("fake_cc").program()), "--spec", spec});
        for (const auto &arg : extra) command.arg(arg);
        return command.env("FAKE_CC_LOG", log.string()).timeout(120);
    }
    std::string run(std::initializer_list<lh::Arg> extra = {}) { return t.ok(command(extra)).out; }
    lh::fs::path cache(const std::string &id = "cache-a") const { return root / id / "exe"; }
    std::vector<std::string> deliveries(const std::string &out) const {
        std::vector<std::string> rows;
        std::istringstream lines(out);
        std::string line;
        while (std::getline(lines, line)) if (line.rfind("deliver ", 0) == 0) rows.push_back(line.substr(8));
        return rows;
    }
    size_t compiler_calls() const {
        if (!lh::fs::exists(log)) return 0;
        const auto text = lh::read_file(log);
        return size_t(std::count(text.begin(), text.end(), '\n'));
    }
    size_t files(const std::string &folder, const std::string &suffix) const {
        size_t n = 0;
        if (!lh::fs::exists(cache() / folder)) return 0;
        for (const auto &entry : lh::fs::directory_iterator(cache() / folder)) {
            const auto name = entry.path().filename().string();
            if (name.size() >= suffix.size() && name.compare(name.size() - suffix.size(), suffix.size(), suffix) == 0) ++n;
        }
        return n;
    }
};

std::string joined(const std::vector<std::string> &rows) {
    std::string text;
    for (const auto &row : rows) text += row + "\n";
    return text;
}

std::string module_suffix(const Build &b) {
    // A Windows harness under Wine names its modules as Windows does.
    for (const auto &entry : lh::fs::directory_iterator(b.cache() / "modules"))
        if (entry.path().extension() == ".dll") return ".dll";
    return ".so";
}

const char *const kFour =
    "-1 0x1e 0 main\n"
    "2 0x02 0 field\n"
    "34 0x04 0 field2\n"
    "130 0x02 1 effect1\n"
    "131 0x04 1 effect2\n";
}

LH_TEST(test_builds_every_set_once_per_distinct_code) {
    Build b(t);
    // The same overlay on two discs is two sets but one module.
    b.sets("-1 0x1e 0 main\n2 0x02 0 field\n34 0x04 0 field\n130 0x02 1 effect1\n");
    const auto out = b.run({"--workers", 2});
    LH_CONTAINS(out, "state done sets 4 ready 4 failed 0 compiled 3 shared 1 cached 0\n");
    auto rows = b.deliveries(out);
    std::sort(rows.begin(), rows.end());
    LH_TEXT_EQ(joined(rows), "effect1 130\nfield 2\nmain -1\n");
    const auto suffix = module_suffix(b);
    LH_EQ(b.files("modules", suffix), size_t(3));
    LH_EQ(b.files("modules", ".ok"), size_t(3));
    LH_EQ(b.files("sets", ""), size_t(4));
    LH_EQ(b.files("work", ""), size_t(0));
    LH_CHECK_MSG(lh::read_file(b.cache() / "include" / "module.h") == "/* header */\n", "headers are written for the compiler");
}

LH_TEST(test_warm_cache_runs_no_compiler) {
    Build b(t);
    b.sets(kFour);
    b.run({"--workers", 2});
    const size_t calls = b.compiler_calls();
    LH_CHECK_MSG(calls > 0, "the cold run compiles");
    const auto out = b.run({"--workers", 2});
    LH_CONTAINS(out, "state done sets 5 ready 5 failed 0 compiled 0 shared 0 cached 5\n");
    LH_EQ(b.compiler_calls(), calls);
    LH_EQ(b.deliveries(out).size(), size_t(5));
    // The executable is handed over first, before any overlay set.
    LH_EQ(b.deliveries(out).front(), std::string("main -1"));
}

LH_TEST(test_order_executable_disc_game_sets_effects_then_the_rest) {
    Build b(t);
    b.sets("-1 0x1e 0 main\n"
           "130 0x04 1 effect-two\n"
           "131 0x02 1 effect-one\n"
           "34 0x04 0 field-two\n"
           "2 0x02 0 field-one\n"
           "4 0x02 0 menu-one\n"
           "66 0x08 0 field-three\n");
    const auto rows = b.deliveries(b.run({"--workers", 1, "--disc", 2}));
    LH_TEXT_EQ(joined(rows), "main -1\nfield-two 34\neffect-two 130\nfield-one 2\nmenu-one 4\nfield-three 66\neffect-one 131\n");
}

LH_TEST(test_running_code_moves_its_set_to_the_front) {
    Build b(t);
    b.sets("-1 0x1e 0 main slow\n"
           "2 0x02 0 field slow\n"
           "4 0x02 0 menu slow\n"
           "130 0x02 1 effect-a slow\n"
           "131 0x10 1 effect-b slow pc=801a0000\n");
    // effect-b is on another disc and would come last; the guest runs it.
    const auto rows = b.deliveries(b.run({"--workers", 1, "--disc", 1, "--ran", "801a0000"}));
    LH_EQ(rows.size(), size_t(5));
    LH_EQ(rows[0], std::string("main -1"));
    const auto at = std::find(rows.begin(), rows.end(), std::string("effect-b 131")) - rows.begin();
    LH_CHECK_MSG(at <= 2, "effect-b was built at position " + std::to_string(at));
}

LH_TEST(test_failed_set_is_recorded_retried_once_then_left_interpreted) {
    Build b(t);
    b.sets("-1 0x1e 0 main\n2 0x02 0 field\n4 0x02 0 broken fail\n");
    auto out = b.run({"--workers", 2});
    LH_CONTAINS(out, "state done sets 3 ready 2 failed 1 ");
    lh::fs::path record;
    for (const auto &entry : lh::fs::directory_iterator(b.cache() / "failed")) record = entry.path();
    LH_CHECK_MSG(!record.empty(), "a failure record exists");
    auto text = lh::read_file(record);
    LH_CONTAINS(text, "attempts 1\n");
    LH_CONTAINS(text, "error: forced failure");
    LH_CHECK_MSG(text.find("SOURCE-TEXT") == std::string::npos, "the record holds diagnostics, never source");
    LH_CHECK_MSG(text.find(b.root.string()) == std::string::npos, "the record names no build folder");

    // The next launch tries once more.
    const size_t before = b.compiler_calls();
    out = b.run({"--workers", 2});
    LH_CONTAINS(out, "state done sets 3 ready 2 failed 1 ");
    LH_CHECK_MSG(b.compiler_calls() > before, "the failed set is retried on the next launch");
    LH_CONTAINS(lh::read_file(record), "attempts 2\n");

    // After that it stays interpreted and costs nothing.
    const size_t after = b.compiler_calls();
    out = b.run({"--workers", 2});
    LH_CONTAINS(out, "state done sets 3 ready 2 failed 1 compiled 0 ");
    LH_EQ(b.compiler_calls(), after);
}

LH_TEST(test_retry_that_succeeds_clears_the_record) {
    Build b(t);
    b.sets("-1 0x1e 0 main\n4 0x02 0 menu linkfail\n");
    LH_CONTAINS(b.run(), "failed 1 ");
    LH_CONTAINS(lh::read_file(*lh::fs::directory_iterator(b.cache() / "failed")), "forced link failure");
    b.sets("-1 0x1e 0 main\n4 0x02 0 menu\n");
    LH_CONTAINS(b.run(), "state done sets 2 ready 2 failed 0 compiled 1 ");
    LH_EQ(b.files("failed", ""), size_t(0));
}

LH_TEST(test_extra_support_units_are_compiled_into_every_module) {
    Build b(t);
    b.sets("-1 0x1e 0 main\n2 0x02 0 field\n");
    b.run({"--extra-unit"});
    const auto log = lh::read_file(b.log);
    size_t extras = 0;
    for (size_t at = log.find("compile extra.c"); at != std::string::npos; at = log.find("compile extra.c", at + 1)) ++extras;
    LH_EQ(extras, size_t(2));
}

LH_TEST(test_killed_builder_leaves_a_cache_the_next_launch_completes) {
    Build b(t);
    b.sets("-1 0x1e 0 main slow\n2 0x02 0 field slow units=3\n4 0x02 0 menu slow units=3\n130 0x02 1 effect slow units=3\n");
    // Leftovers of a crash at the worst moments: a half-published module and a build in progress.
    const auto stale = std::string(64, 'a');
    lh::fs::create_directories(b.cache() / "modules");
    lh::fs::create_directories(b.cache() / "work" / "123-1");
    lh::write_file(b.cache() / "modules" / (stale + ".so"), "half");
    lh::write_file(b.cache() / "modules" / (stale + ".dll"), "half");
    lh::write_file(b.cache() / "work" / "123-1" / "unit_000.c", "old");
    const auto killed = t.run(b.command({"--workers", 2, "--kill-after-ms", 1500}));
    LH_CHECK_MSG(killed.rc != 0, "the harness was killed");
    for (const auto &entry : lh::fs::directory_iterator(b.cache() / "modules")) {
        const auto name = entry.path().filename().string();
        LH_CHECK_MSG(name.rfind(stale, 0) != 0, "an unreceipted module is removed: " + name);
    }
    const auto out = b.run({"--workers", 2});
    LH_CONTAINS(out, "state done sets 4 ready 4 failed 0 ");
    LH_EQ(b.files("work", ""), size_t(0));
    LH_EQ(b.files("modules", ".ok"), size_t(4));
    LH_EQ(b.files("modules", module_suffix(b)), size_t(4));
}

LH_TEST(test_two_players_share_one_cache) {
    Build b(t);
    b.sets("-1 0x1e 0 main slow\n2 0x02 0 field slow units=2\n4 0x02 0 menu slow units=2\n130 0x02 1 effect slow units=2\n");
    const auto twin = t.temp() / "twin.txt";
    // Both start together: one builds, the other loads what it publishes.
    const auto out = b.run({"--workers", 1, "--twin", "--twin-out", twin});
    const auto other = lh::read_file(twin);
    LH_CONTAINS(out, "state done sets 4 ready 4 failed 0 ");
    LH_CONTAINS(other, "state done sets 4 ready 4 failed 0 ");
    const auto a = std::stoi(lh::capture(out, "compiled ([0-9]+)")), c = std::stoi(lh::capture(other, "compiled ([0-9]+)"));
    LH_EQ(a + c, 4);
    LH_CHECK_MSG(a == 0 || c == 0, "only one player compiles");
    LH_EQ(b.deliveries(out).size(), size_t(4));
    LH_EQ(b.deliveries(other).size(), size_t(4));
    LH_EQ(b.files("modules", ".ok"), size_t(4));
}

LH_TEST(test_unused_cache_folders_of_other_versions_are_removed) {
    Build b(t);
    b.sets("-1 0x1e 0 main\n");
    b.run({"--cache-id", "cache-old"});
    LH_CHECK_MSG(lh::fs::exists(b.root / "cache-old"), "the first version's folder exists");
    // While a player of the old version runs, its folder stays.
    const auto twin = t.temp() / "twin.txt";
    b.run({"--cache-id", "cache-new", "--delay-ms", 700, "--twin", "--twin-out", twin, "--twin-cache-id", "cache-old",
           "--twin-hold-ms", 3000});
    LH_CONTAINS(lh::read_file(twin), "state done ");
    LH_CHECK_MSG(lh::fs::exists(b.root / "cache-old" / "exe" / "sets"), "a folder in use is kept");
    b.run({"--cache-id", "cache-new"});
    LH_CHECK_MSG(!lh::fs::exists(b.root / "cache-old"), "an unused folder is removed");
    LH_CHECK_MSG(lh::fs::exists(b.root / "cache-new" / "exe" / "sets"), "the new folder is complete");
}

LH_TEST(test_unwritable_cache_stops_building_without_crashing) {
    Build b(t);
    b.sets(kFour);
    // A file where the cache folder belongs: nothing can be created under it.
    lh::fs::create_directories(b.root);
    lh::write_file(b.root / "cache-a", "not a folder");
    const auto out = b.run();
    LH_CONTAINS(out, "state stopped ");
    LH_CONTAINS(out, "error cannot ");
    LH_EQ(b.deliveries(out).size(), size_t(0));
}

LH_TEST(test_compiler_that_cannot_start_stops_building) {
    Build b(t);
    b.sets(kFour);
    auto command = t.harness("native_build");
    command.args({"--root", b.root, "--compiler", t.temp() / "no-such-compiler", "--spec", b.spec});
    const auto out = t.ok(command.timeout(60)).out;
    LH_CONTAINS(out, "state stopped ");
    LH_CONTAINS(out, "error cannot run the compiler");
    // Nothing is blamed on a set when the compiler itself is missing.
    LH_EQ(b.files("failed", ""), size_t(0));
}

LH_TEST(test_unreadable_discs_stop_building_but_keep_the_cache) {
    Build b(t);
    b.sets(kFour);
    b.run();
    b.sets(std::string(kFour) + "200 0x02 1 effect-new\n");
    auto command = b.command();
    const auto out = t.ok(command.env("HARNESS_OPEN_FAILS", "1")).out;
    LH_CONTAINS(out, "state stopped sets 6 ready 5 ");
    LH_CONTAINS(out, "error cannot read the discs");
}

LH_TEST(test_module_the_loader_refuses_is_recorded_and_rebuilt_later) {
    Build b(t);
    b.sets("-1 0x1e 0 main\n2 0x02 0 field\n");
    auto out = t.ok(b.command().env("HARNESS_REFUSE", "field")).out;
    LH_CONTAINS(out, "state done sets 2 ready 1 failed 1 ");
    LH_CONTAINS(out, "error module for field refused: refused by the loader");
    LH_EQ(b.files("modules", ".ok"), size_t(1));
    LH_CONTAINS(lh::read_file(*lh::fs::directory_iterator(b.cache() / "failed")), "attempts 1\n");
    // The retry on the next launch builds it again, and this time the loader takes it.
    out = b.run();
    LH_CONTAINS(out, "state done sets 2 ready 2 failed 0 compiled 1 ");
    LH_EQ(b.files("failed", ""), size_t(0));
}

LH_TEST(test_relaunch_resumes_an_interrupted_module) {
    Build b(t);
    b.sets("-1 0x1e 0 main slow units=8\n");
    const auto killed = t.run(b.command({"--workers", 1, "--kill-after-ms", 1800}));
    LH_CHECK_MSG(killed.rc != 0, "the harness was killed");
    const size_t before = b.compiler_calls();
    LH_CHECK_MSG(before >= 2 && before < 9, "some units compiled before the kill: " + std::to_string(before));
    LH_CONTAINS(b.run({"--workers", 1}), "state done sets 1 ready 1 failed 0 compiled 1 ");
    // Units finished before the kill are not compiled again; the one cut short is.
    const size_t after = b.compiler_calls() - before;
    LH_CHECK_MSG(after + before <= 9 + 1 + 1, "compiler calls before " + std::to_string(before) + ", after " + std::to_string(after));
    LH_EQ(b.files("work", ""), size_t(0));
}

LH_TEST(test_quitting_keeps_what_was_compiled) {
    Build b(t);
    b.sets("-1 0x1e 0 main slow units=8\n");
    LH_CONTAINS(b.run({"--workers", 1, "--stop-after-ms", 1800}), "state building ");
    const size_t before = b.compiler_calls();
    LH_CHECK_MSG(b.files("work", "") == 1, "the interrupted build is kept");
    LH_CONTAINS(b.run({"--workers", 1}), "state done sets 1 ready 1 failed 0 compiled 1 ");
    const size_t after = b.compiler_calls() - before;
    LH_CHECK_MSG(before + after <= 9 + 1 + 1, "compiler calls before " + std::to_string(before) + ", after " + std::to_string(after));
}

namespace {
// A zombie no longer runs; it only waits for whoever adopted it to collect it.
bool running(const std::string &pid) {
    const lh::fs::path stat = "/proc/" + pid + "/stat";
    if (!lh::fs::exists(stat)) return false;
    const auto text = lh::read_file(stat);
    const auto end = text.rfind(')');
    return end != std::string::npos && end + 2 < text.size() && text[end + 2] != 'Z';
}
}

LH_TEST(test_a_killed_player_leaves_no_compiler_running) {
    // As zig cc does, each fake compile runs its work in a child of its own,
    // which the driver's death signal alone would leave running. A job object
    // does this on Windows, where the fake compiler starts no child.
    if (lh::internal::build_info().windows_paths) lh::skip("process groups and the reaper are Linux's");
    Build b(t);
    b.sets("-1 0x1e 0 main child units=4\n2 0x02 0 field child units=4\n");
    const auto pids = t.temp() / "pids.txt";
    const auto killed = t.run(b.command({"--workers", 2, "--kill-after-ms", 1500}).env("FAKE_CC_PIDS", pids.string()));
    LH_CHECK_MSG(killed.rc != 0, "the harness was killed");
    LH_CHECK_MSG(lh::fs::exists(pids), "compilers started children before the kill");
    std::vector<std::string> children;
    std::istringstream list(lh::read_file(pids));
    for (std::string pid; std::getline(list, pid);) if (!pid.empty()) children.push_back(pid);
    LH_CHECK(!children.empty());
    for (const auto &pid : children) {
        for (int wait = 0; wait < 100 && running(pid); ++wait) std::this_thread::sleep_for(std::chrono::milliseconds(50));
        LH_CHECK_MSG(!running(pid), "compiler child " + pid + " outlived the player");
    }
}
