// Generator rules added after the port: per-set entry hints, code prefixes,
// hinted zero functions and the fixes to defects carried over from the original.
#include "generator_cases.h"

namespace {
using namespace generator_cases;
// A disc whose IMG holds one uncompressed overlay after its top table, then one
// sector per asset, and the full catalog that describes them; overlays.json
// carries `extra` as well.
using Assets = std::vector<std::pair<std::string, std::string>>;
struct OverlayDisc {
    lh::fs::path cue, catalog;
    std::string sha1;
};
OverlayDisc overlay_disc(lh::Case &t, const std::string &payload, const O &extra, const Assets &assets = {}) {
    auto data = disc(), exe = valid_exe();
    int img = int(2 + assets.size()) * 2048;
    auto put = [&](int sector, const std::string &s) {
        for (size_t i = 0; i < s.size(); i += 2048)
            data.replace((sector + i / 2048) * 2352 + 24, std::min<size_t>(2048, s.size() - i), s.substr(i, 2048));
    };
    put(18, directory_record("SLUS_008.92;1", 20, exe.size()) + directory_record("FF8DISC1.IMG;1", 24, img));
    put(20, exe);
    put(24, pack(Words(266, 1)));
    put(25, payload);
    A entries = {O{{"path", "data/x.ovl"}, {"locator", O{{"table", "battle"}, {"index", 0}}}}};
    for (size_t at = 0; at < assets.size(); ++at) {
        put(26 + int(at), assets[at].second);
        entries.push_back(O{{"path", assets[at].first},
                            {"sector", num(2 + at)},
                            {"size", num(assets[at].second.size())},
                            {"sha1", sha1(assets[at].second)},
                            {"locator", O{{"table", "battle"}, {"index", num(1 + at)}}}});
    }
    OverlayDisc out{t.temp() / "ff8 (Disc 1).cue", t.temp() / "catalog", sha1(payload)};
    lh::write_file(t.temp() / "ff8.bin", data);
    lh::write_file(out.cue, "FILE \"ff8.bin\" BINARY\n");
    lh::fs::create_directories(out.catalog);
    O metadata = {{"exe_sha1", sha1(exe)}, {"main_code_ranges", A{A{"0x80010000", "0x80010400"}}}};
    for (const auto &item : extra)
        metadata[item.first] = item.second;
    lh::write_file(out.catalog / "overlays.json", J(metadata).dump());
    auto size = num(payload.size());
    lh::write_file(out.catalog / "files-disc1.json",
                   J(O{{"exe", O{{"name", "SLUS_008.92"}}},
                       {"image", O{{"lba", 24}, {"size", img}}},
                       {"entries", entries}})
                       .dump());
    lh::write_file(out.catalog / "overlays-all.json",
                   J(O{{"overlays", A{O{{"name", "x"},
                                        {"names", A{"x"}},
                                        {"paths", A{"data/x.ovl"}},
                                        {"sha1", out.sha1},
                                        {"size", size},
                                        {"locations", A{O{{"disc", 1},
                                                          {"path", "data/x.ovl"},
                                                          {"sector", 1},
                                                          {"size", size},
                                                          {"read_size", size},
                                                          {"compressed", false},
                                                          {"sha1_stored", out.sha1},
                                                          {"loaded_sha1", out.sha1},
                                                          {"loaded_size", size},
                                                          {"load_address", "0x80098000"}}}}}}}})
                       .dump());
    return out;
}
} // namespace

LH_TEST(test_set_entry_points) {
    // The overlay's entry returns at once; the function at +16 is only
    // reached through a pointer, so only its hint makes it native.
    auto payload = pack(RET + Words{0, 0, imm(9, 0, 2, 7)} + RET);
    auto hinted = [&](A points, std::string sha1 = "") {
        return O{{"set_entry_points", A{O{{"name", "x"}, {"sha1", sha1}, {"entry_points", points}}}}};
    };
    auto generate = [&](const OverlayDisc &d, const char *shape) {
        auto out = t.temp() / (std::string("out-") + shape);
        lh::fs::remove_all(out);
        t.ok(t.harness("lionheart-gen")
                 .args({"--disc", d.cue, "--catalog", d.catalog, "--shape", shape, "-o", out})
                 .cwd(t.temp())
                 .timeout(120));
        return out;
    };
    auto plan = [&](const OverlayDisc &d) {
        return t.ok(t.harness("lionheart-gen")
                        .args({"--disc", d.cue, "--catalog", d.catalog, "--shape", "modules", "--plan"})
                        .cwd(t.temp())
                        .timeout(120))
            .out;
    };
    auto plain = overlay_disc(t, payload, O{});
    auto before = plan(plain);
    LH_NOT_CONTAINS(lh::read_file(generate(plain, "linked") / "overlay_128_000.c"), "f_80098010");
    auto disc = overlay_disc(t, payload, hinted(A{"0x80098010"}, plain.sha1));
    LH_CONTAINS(lh::read_file(generate(disc, "linked") / "overlay_128_000.c"), "void ov128_f_80098010(cpu_t *c)");
    LH_CONTAINS(lh::read_file(generate(disc, "modules") / "modules/set-128/unit_000.c"), "void f_80098010(cpu_t *c)");
    // Only the hinted set's identity changes; the executable's does not.
    auto after = plan(disc);
    LH_TEXT_EQ(before.substr(0, before.find('\n')), after.substr(0, after.find('\n')));
    LH_CHECK(before != after);
    // Spelling and order are metadata, checked before any disc is read;
    // whether a hint is an instruction needs the image.
    auto refuse = [&](A points, const std::string &sha1, const std::string &message, bool planned = true) {
        auto d = overlay_disc(t, payload, hinted(points, sha1));
        LH_CONTAINS(cli(t, {"--disc", d.cue, "--catalog", d.catalog, "-o", t.temp() / "refused"}), message);
        if (planned)
            LH_CONTAINS(cli(t, {"--disc", d.cue, "--catalog", d.catalog, "--shape", "modules", "--plan"}), message);
    };
    refuse(A{"0x80098008"}, disc.sha1, "entry point 0x80098008 is not an instruction", false);
    refuse(A{"0x80098020"}, disc.sha1, "entry point 0x80098020 is not an instruction", false);
    refuse(A{"0x80098010", "0x80098000"}, disc.sha1, "ascending order");
    refuse(A{"0x80098012"}, disc.sha1, "ascending order");
    refuse(A{"0x8009801C"}, disc.sha1, "eight lowercase digits");
    refuse(A{}, disc.sha1, "lists no entry points");
    refuse(A{"0x80098010"}, std::string(40, '0'), "does not name one catalog set");
    // Discovery itself takes a set's hints as roots, in either emission mode.
    auto ov = image(payload, 0x80098000);
    ov["index"] = 128;
    ov["entry_points"] = A{num(0x80098010)};
    ov["scan_calls"] = false;
    for (auto mode : {"fast", "trace"})
        parity(t, O{{"main", image(pack(RET))}, {"overlays", A{ov}}, {"mode", mode}}, std::string("set-hints-") + mode);
    return;
}

LH_TEST(test_branch_into_a_delay_slot) {
    // The beq lands on the jump's delay slot at +16; from there the code
    // runs on to +20 and its own return, reached no other way.
    auto main = image(Words{imm(4, 4, 0, 3), 0, imm(9, 2, 2, 1), j(2, BASE + 0x24), imm(9, 2, 2, 2), imm(9, 2, 2, 3)} + RET +
                      Words{0} + RET);
    main["seeds"] = A{num(BASE)};
    main["scan_calls"] = false;
    for (auto v : std::vector<std::pair<std::string, std::string>>{{"fast", "precise"}, {"trace", "fast"}}) {
        auto spec = main_spec(main);
        spec["mode"] = v.first;
        spec["host_mode"] = v.second;
        LH_CONTAINS(source(parity(t, spec, "delay-slot-target-" + v.first)), "case 0x80010014u:");
    }
    return;
}

LH_TEST(test_jump_table_scan_keeps_r0_zero) {
    // `lui zero` is a no-op; the table base is then moved through r0, so a
    // scanner that let r0 take the constant would look for the table elsewhere.
    uint32_t table = BASE + 0x400;
    Words w = {imm(15, 0, 0, 0x10),
               imm(15, 0, 8, (table + 0x8000) >> 16),
               imm(9, 8, 8, table & 65535),
               r(33, 8, 0, 10),
               r(0, 0, 4, 9, 2),
               r(33, 9, 10, 9),
               imm(35, 9, 9, 0),
               0,
               r(8, 9),
               0};
    std::string data(0x800, '\0');
    data.replace(0, pack(w).size(), pack(w));
    for (int i = 0; i < 4; ++i) {
        put32(data, 0x400 + i * 4, BASE + 0x40 + i * 8);
        data.replace(0x40 + i * 8, 8, pack(RET));
    }
    auto main = image(data);
    main["seeds"] = A{num(BASE)};
    main["scan_calls"] = false;
    auto out = parity(t, main_spec(main), "r0-table");
    auto manifest = J::parse(lh::read_file(out / "manifest.json"));
    LH_CHECK(manifest["jump_tables"].contains("0x80010020"));
    return;
}

LH_TEST(test_call_ticks_see_the_callee_window) {
    // A function that calls itself: the call's tick knows the window at
    // its target, the entry, as a jump to it would.
    auto main = image(Words{imm(4, 4, 0, 4), imm(9, 2, 2, 1), imm(9, 4, 4, -1), j(3, BASE), 0} + RET);
    main["seeds"] = A{num(BASE)};
    main["scan_calls"] = false;
    auto spec = main_spec(main);
    spec["host_mode"] = "fast";
    LH_CONTAINS(source(parity(t, spec, "self-call")),
                "c->pc = 0x80010000u;\nc_count_recompiled(c, 0x80010000u, 2 - ticked);\nc_recomp_budget(c, 2 - ticked);\n"
                "if (!c_tick_window(c, 2 - ticked, 2)) return;");
    return;
}

LH_TEST(test_register_jumps_make_no_leaders) {
    // The return at +8 reads as an offset of 8 words to +44, which is in the
    // function; only real branch targets and fall-through points resume.
    Words w = {imm(4, 0, 0, 3), 0, r(8, 31), 0};
    for (int n = 0; n < 7; ++n)
        w.push_back(imm(9, 2, 2, 1));
    w = w + Words{imm(9, 2, 2, 2), j(2, BASE + 8), 0};
    auto main = image(w);
    main["seeds"] = A{num(BASE)};
    main["scan_calls"] = false;
    for (auto v : std::vector<std::pair<std::string, std::string>>{{"fast", "precise"}, {"fast", "fast"}, {"trace", "fast"}}) {
        auto spec = main_spec(main);
        spec["mode"] = v.first;
        spec["host_mode"] = v.second;
        auto out = parity(t, spec, "register-jump-" + v.first + "-" + v.second);
        auto tables = lh::read_file(out / "dispatch_table.c");
        LH_CONTAINS(tables, "{0x80010010u, f_80010000},");
        LH_NOT_CONTAINS(tables, "{0x8001002cu, f_80010000},");
    }
    return;
}

LH_TEST(test_hinted_zero_functions) {
    // A caller and, at +0x100, a delay function: `zeros` no-ops, then
    // `last` (a return when it is one) and a zero delay slot.
    auto program = [&](size_t zeros, uint32_t last, bool hinted) {
        Words w = Words{j(3, BASE + 0x100), 0} + RET;
        w.resize(0x40, 0);
        w.resize(0x40 + zeros, 0);
        w.push_back(last);
        w.push_back(0);
        w = w + RET;
        auto main = image(w);
        main["seeds"] = A{num(BASE)};
        main["scan_calls"] = false;
        if (hinted)
            main["entry_points"] = A{num(BASE + 0x100)};
        return main;
    };
    auto translated = [&](const O &main, const std::string &label) {
        return source(emit(t, main_spec(main), label)).find("void f_80010100(cpu_t *c)") != std::string::npos;
    };
    auto out = parity(t, main_spec(program(64, r(8, 31), true)), "zero-function-fast");
    LH_CONTAINS(source(out), "case 0x80010204u:");
    auto trace = main_spec(program(64, r(8, 31), true));
    trace["mode"] = "trace";
    parity(t, trace, "zero-function-trace");
    // Without the hint the call alone does not make zeros code; nor does a
    // hinted run that ends in anything but a return, or ends past 8 KiB.
    LH_CHECK(!translated(program(64, r(8, 31), false), "unhinted"));
    LH_CHECK(!translated(program(64, imm(9, 0, 2, 1), true), "no-return"));
    LH_CHECK(translated(program(2046, r(8, 31), true), "within-limit"));
    LH_CHECK(!translated(program(2048, r(8, 31), true), "past-limit"));
    return;
}

LH_TEST(test_code_prefixes) {
    // Stage-like assets: code at the load address, data after it. Two
    // assets share their code and differ in data; the third differs in one
    // constant of its entry function. Past the 64-byte limit, data holds a
    // word shaped like a prologue that discovery must not reach.
    const uint32_t base = 0x8013e000;
    auto code = [&](int constant, uint32_t data_word) {
        Words w = {imm(9, 29, 29, -8), j(3, base + 0x20), 0, imm(15, 0, 2, constant), r(8, 31), imm(9, 29, 29, 8), 0, 0,
                   imm(9, 29, 29, -8), r(8, 31), imm(9, 29, 29, 8)};
        w.resize(16, 0);
        w.push_back(data_word);
        w.resize(20, 0);
        w = w + Words{imm(9, 29, 29, -8), r(8, 31), imm(9, 29, 29, 8)};
        return pack(w);
    };
    // A literal-only LZS stream: a flag byte of eight literals per eight bytes.
    auto lzs = [](const std::string &plain) {
        std::string body;
        for (size_t at = 0; at < plain.size(); at += 8)
            body += char(0xff) + plain.substr(at, 8);
        std::string header(4, '\0');
        put32(header, 0, uint32_t(body.size()));
        return header + body;
    };
    Assets assets = {{"data/stage0.x", lzs(code(0x8014, 1))},
                     {"data/stage1.x", lzs(code(0x8014, 2))},
                     {"data/stage2.x", lzs(code(0x8015, 1))}};
    auto first = code(0x8014, 1).substr(0, 44), second = code(0x8015, 1).substr(0, 44);
    auto record = [&](A variants, int limit = 64) {
        return O{{"code_prefixes", A{O{{"name", "stage"}, {"load_address", "0x8013e000"}, {"limit", limit}, {"variants", variants}}}}};
    };
    auto variant = [](const std::string &digest, int size, A paths) {
        return O{{"sha1", digest}, {"size", size}, {"paths", paths}};
    };
    A variants = {variant(sha1(first), 44, A{"data/stage0.x", "data/stage1.x"}), variant(sha1(second), 44, A{"data/stage2.x"})};
    auto payload = pack(RET);
    auto run = [&](const OverlayDisc &d, std::initializer_list<lh::Arg> args) {
        auto command = t.harness("lionheart-gen").args({"--disc", d.cue, "--catalog", d.catalog});
        return t.ok(command.args(args).cwd(t.temp()).timeout(120)).out;
    };
    auto d = overlay_disc(t, payload, record(variants), assets);
    // The catalog's one group is set 128, so the record is set 129 and its
    // second variant 130: two images, one overlay row, one module.
    run(d, {"-o", t.temp() / "linked"});
    auto tables = lh::read_file(t.temp() / "linked/dispatch_table.c");
    LH_CONTAINS(tables, "const size_t generated_overlay_count = 2;");
    LH_CONTAINS(tables, "{129, 0x8013e000u, 44, {");
    LH_CONTAINS(tables, "{0x8013e000u, ov129_f_8013e000},\n{0x8013e000u, ov130_f_8013e000},");
    LH_CONTAINS(tables, "{0x8013e020u, ov129_f_8013e020},");
    LH_NOT_CONTAINS(tables, "ov130_f_8013e020");
    LH_NOT_CONTAINS(tables, "ov130_entries");
    LH_NOT_CONTAINS(tables, "0x8013e050u");
    auto manifest = J::parse(lh::read_file(t.temp() / "linked/manifest.json"));
    LH_EQ(manifest["overlays"].size(), 3u);
    LH_CHECK(manifest["overlays"][2]["row"] == J(129));
    run(d, {"--shape", "modules", "-o", t.temp() / "modules"});
    auto module = lh::read_file(t.temp() / "modules/modules/set-129/tables.c");
    LH_CONTAINS(module, "{0x8013e000u, f_8013e000},\n{0x8013e000u, f_8013e000_1},");
    LH_CONTAINS(module, "{0x8013e020u, f_8013e020},");
    LH_NOT_CONTAINS(module, "f_8013e020_1");
    LH_CHECK(!lh::fs::exists(t.temp() / "modules/modules/set-130"));
    auto plan = run(d, {"--shape", "modules", "--plan"});
    LH_CONTAINS(plan, " discs=2 effect=0 base=8013e000 size=44 stage\n");
    LH_CHECK(plan.find("\n129 ") != std::string::npos);
    LH_CONTAINS(run(d, {"--shape", "modules", "--emit-set", "129", "-o", t.temp() / "single"}), "129 ");
    LH_TEXT_EQ(lh::read_file(t.temp() / "single/set-129/tables.c"), module);
    // Refusals: what the disc does not bear out, and malformed records.
    auto refuse = [&](const O &extra, const std::string &message, bool planned) {
        auto bad = overlay_disc(t, payload, extra, assets);
        LH_CONTAINS(cli(t, {"--disc", bad.cue, "--catalog", bad.catalog, "-o", t.temp() / "refused"}), message);
        if (planned)
            LH_CONTAINS(cli(t, {"--disc", bad.cue, "--catalog", bad.catalog, "--shape", "modules", "--plan"}), message);
    };
    refuse(record(A{variants[0], variant(sha1(second), 40, A{"data/stage2.x"})}), "data/stage2.x on disc 1 starts with 44 bytes of code with SHA-1 " + sha1(second), false);
    refuse(record(variants, 128), "data/stage0.x on disc 1 starts with 92 bytes of code", false);
    refuse(record(A{variants[0], variant(sha1(second), 44, A{"data/stage2.x", "data/stage3.x"})}), "stage: disc 1 holds only some of its assets", true);
    refuse(record(A{variants[0], variant(sha1(second), 44, A{"data/stage0.x"})}), "code_prefixes: data/stage0.x is listed twice", true);
    refuse(record(variants, 66), "needs an aligned load address", true);
    refuse(record(A{variant(sha1(first), 128, A{"data/stage0.x", "data/stage1.x"}), variants[1]}), "needs a SHA-1, an aligned size within the limit", true);
    // The emitted shape of a shared row, frozen like every other output.
    auto image_of = [&](const std::string &bytes, int index, bool shares) {
        auto ov = image(bytes, base);
        ov["index"] = index;
        ov["seeds"] = A{num(base), num(base + 0x20)};
        ov["scan_calls"] = false;
        ov["shares_row"] = shares;
        return ov;
    };
    for (auto mode : {"fast", "trace"})
        parity(t, O{{"main", image(pack(RET))}, {"overlays", A{image_of(first, 129, false), image_of(second, 130, true)}}, {"mode", mode}},
               std::string("prefix-row-") + mode);
    return;
}

LH_TEST(test_lowering_leaves_strings_alone) {
    // Every return spills the cached registers first; a return spelled
    // inside a stop reason is text and must reach the log as written.
    std::string text = "void f_80010000(cpu_t *c) {\nc->r[1]++;\ncpu_stop(c, \"stop; return; \\\" return;\");\nreturn;\n}\n";
    auto out = parity(t, O{{"lower_source", text}, {"max_registers", 1}}, "strings");
    auto lowered = lh::read_file(out / "lowered.c");
    LH_CONTAINS(lowered, "cpu_stop(c, \"stop; return; \\\" return;\")");
    LH_CONTAINS(lowered, "\n{ c->r[1] = reg_1; return; }\n}");
    LH_EQ(std::count(lowered.begin(), lowered.end(), '{'), 2);
    return;
}
