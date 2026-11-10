#include "generator_cases.h"
#include "synthetic_disc.h"

namespace {
using namespace generator_cases;
struct Fixture {
    lh::Case &t;
    lh::fs::path catalog, out;
    std::array<lh::fs::path, 2> cues;
    std::string executable = synthetic::exe();
    std::string overlay = pack(RET);
    Fixture(lh::Case &test) : t(test), catalog(t.temp() / "catalog"), out(t.temp() / "output") {
        lh::fs::create_directories(catalog);
        write();
    }
    void write() {
        O discs;
        for (int n = 1; n <= 2; ++n) {
            synthetic::Disc d;
            d.number = n;
            d.executable = executable;
            auto bin = t.temp() / ("disc" + std::to_string(n) + ".bin");
            synthetic::disc(bin, d);
            auto raw = lh::read_file(bin);
            raw.replace(827 * 2352 + 24, overlay.size(), overlay);
            lh::write_file(bin, raw);
            cues[n-1] = t.temp() / ("disc" + std::to_string(n) + ".cue");
            lh::write_file(cues[n-1], "FILE \"" + bin.filename().string() + "\" BINARY\n");
            discs[std::to_string(n)] = O{{"exe", synthetic::serial(n)}, {"image", "FF8DISC" + std::to_string(n) + ".IMG"},
                {"overlays", A{O{{"index", 4}, {"img_sector", 1}, {"stored_size", num(overlay.size())},
                    {"size", num(overlay.size())}, {"compressed", false}, {"load_address", "0x80098000"}, {"sha1", sha1(overlay)}}}}};
        }
        lh::write_file(catalog / "overlays.json", J(O{{"exe_sha1", sha1(executable)},
            {"main_code_ranges", A{A{"0x80010000", "0x80010800"}}}, {"discs", discs}}).dump());
        lh::write_file(catalog / "exe-identity.json", J(O{{"file_size", num(executable.size())}, {"sha1", sha1(executable)}}).dump());
    }
    lh::Result run(const std::string &mode = "good", bool two = false) {
        auto cmd = t.harness("driver").args({catalog, out, mode, cues[0]});
        if (two) cmd.arg(cues[1]);
        return t.run(cmd);
    }
    void good(bool hit, bool two = false) {
        auto r = run("good", two);
        LH_EQ(r.rc, 0);
        LH_CONTAINS(r.out, hit ? "hit=1" : "hit=0");
        if (hit) { LH_NOT_CONTAINS(r.out, "generation "); LH_NOT_CONTAINS(r.out, "emit "); }
        else { LH_CONTAINS(r.out, "generation "); LH_CONTAINS(r.out, "emit "); }
        LH_CHECK(lh::fs::exists(out / "setup-cache.native"));
    }
};
}

LH_TEST(cache_hit_skips_generation) {
    Fixture f(t);
    f.good(false);
    auto receipt = lh::read_file(f.out / "setup-cache.native");
    f.good(true);
    LH_TEXT_EQ(receipt, lh::read_file(f.out / "setup-cache.native"));
}

LH_TEST(disc_rules) {
    Fixture f(t);
    auto reject = [&](std::initializer_list<lh::Arg> discs, const std::string &message) {
        auto r = t.run(t.harness("lionheart-gen").args({"--cache", "--legacy-overlays", "--catalog", f.catalog, "-o", f.out}).args(discs));
        LH_CHECK(r.rc != 0);
        LH_CONTAINS(r.err, message);
        LH_CHECK(!lh::fs::exists(f.out / "setup-cache.native"));
    };
    reject({"--disc", f.cues[0], "--disc", f.cues[0]}, "same disc");
    reject({"--disc", f.cues[1]}, "disc 1 is required");
    reject({}, "EXE or --disc is required");
    synthetic::Disc other;
    other.executable[2056] ^= 1;
    synthetic::disc(t.temp() / "disc1.bin", other);
    reject({"--disc", f.cues[0]}, "unsupported disc EXE identity");
    f.write();
    f.good(false);
    // A valid receipt must never bypass validation of the current disc.
    synthetic::disc(t.temp() / "disc1.bin", other);
    auto r = f.run();
    LH_CHECK(r.rc != 0);
    LH_CONTAINS(r.err, "unsupported disc EXE identity");
}

LH_TEST_P(output_invalidation, std::vector<std::string>({"changed", "truncated", "missing"})) {
    Fixture f(t);
    f.good(false);
    auto path = f.out / "functions_000.c";
    auto original = lh::read_file(path);
    if (p == "missing") lh::fs::remove(path);
    else if (p == "truncated") lh::write_file(path, original.substr(0, original.size()/2));
    else { auto changed = original; changed[0] ^= 1; lh::write_file(path, changed); }
    f.good(false);
    LH_TEXT_EQ(lh::read_file(path), original);
    f.good(true);
}

LH_TEST_P(input_invalidation, std::vector<std::string>({"catalog", "exe", "overlay", "disc-set", "options", "selection"})) {
    Fixture f(t);
    f.good(false);
    if (p == "catalog") lh::write_file(f.catalog / "extra.json", "{}");
    if (p == "exe") { f.executable[2056] ^= 1; f.write(); }
    if (p == "overlay") { f.overlay = pack(Words{0} + RET); f.write(); }
    if (p == "options" || p == "selection") {
        if (p == "selection") lh::write_file(f.catalog / "play-register-cache.json", "{\"max_registers\":1,\"functions\":[\"f_80010000\"]}");
        auto cmd = t.harness("lionheart-gen").args({"--cache", "--legacy-overlays", "--catalog", f.catalog,
            "--disc", f.cues[0], "-o", f.out, "--host-mode", "fast"});
        if (p == "selection") cmd.args({"--timing", "block"});
        auto r = t.ok(cmd);
        LH_CONTAINS(r.out, "generating native functions");
        LH_CONTAINS(t.ok(cmd).out, "cache hit");
    } else { f.good(false, p == "disc-set"); f.good(true, p == "disc-set"); }
}

LH_TEST_P(cancelled_emission, std::vector<std::string>({"fresh", "previous", "uncached"})) {
    Fixture f(t);
    if (p != "fresh") f.good(false);
    if (p == "previous") lh::write_file(f.catalog / "extra.json", "{}");
    auto r = f.run(p == "uncached" ? "uncached" : "cancel");
    LH_EQ(r.rc, 2);
    LH_CONTAINS(r.out, "emit 1/");
    LH_CHECK(lh::fs::exists(f.out / "functions_000.c"));
    LH_CHECK(!lh::fs::exists(f.out / "setup-cache.native"));
    f.good(false);
    f.good(true);
}

LH_TEST(standalone_checks) {
    t.ok(t.harness("unit").arg(t.temp() / "unit"));
    t.ok(t.host("cmake").args({"-DSETUP=" + (t.root() / "tools/setup.cmake").string(),
        "-P", t.root() / "generator/test/setup_cli.cmake"}));
}

LH_TEST(source_fingerprint) {
    Fixture f(t);
    f.good(false);
    auto source = t.temp() / "generator", build = t.temp() / "build";
    lh::fs::copy(t.root() / "generator", source, lh::fs::copy_options::recursive);
    auto path = source / "src/base.c";
    lh::write_file(path, lh::read_file(path) + "\n/* Source identity regression fixture. */\n");
    t.ok(t.host("cmake").args({"-S", source, "-B", build}));
    t.ok(t.host("nice").args({"-n", "10", "cmake", "--build", build, "--target", "lionheart-gen", "-j2"}).timeout(120));
    auto before = t.ok(t.harness("lionheart-gen").arg("--fingerprint")).out;
    auto after = t.ok(t.host(build / "lionheart-gen").arg("--fingerprint")).out;
    LH_CHECK(before != after);
    auto cmd = t.host(build / "lionheart-gen").args({"--cache", "--legacy-overlays", "--catalog", f.catalog,
        "--disc", f.cues[0], "-o", f.out});
    LH_CONTAINS(t.ok(cmd).out, "generating native functions");
    LH_CONTAINS(t.ok(cmd).out, "cache hit");
    f.good(false);
}

LH_TEST_P(write_failure, std::vector<std::string>({"fresh", "previous"})) {
    Fixture f(t);
    if (p == "previous") f.good(false);
    auto blocked = f.out / "dispatch_table.c";
    lh::fs::remove(blocked);
    lh::fs::create_directories(blocked);
    // A directory at a late output path fails even when tests run as root.
    auto result = f.run();
    LH_EQ(result.rc, 1);
    LH_CONTAINS(result.out, "emit 1/");
    LH_CONTAINS(result.err, "dispatch_table.c");
    LH_CHECK(lh::fs::exists(f.out / "functions_000.c"));
    LH_CHECK(!lh::fs::exists(f.out / "setup-cache.native"));
    lh::fs::remove(blocked);
    f.good(false);
    f.good(true);
}
