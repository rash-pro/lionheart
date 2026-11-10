#include "lh_json.h"
#include "recompiler_fixture.h"
#include <array>
#include <dlfcn.h>
#include <iomanip>

namespace {
using namespace lh::gen;
using J = lh::Json;
using O = J::Object;
using A = J::Array;
constexpr uint32_t BASE = 0x80010000;
J num(uint64_t n) { return J(int64_t(n)); }
std::string hex(const std::string &s) {
    static const char digits[] = "0123456789abcdef";
    std::string out;
    for (unsigned char c : s) {
        out += digits[c >> 4];
        out += digits[c & 15];
    }
    return out;
}
std::string symbol(uint32_t pc, const std::string &prefix = "") {
    std::ostringstream s;
    s << prefix << "f_" << std::hex << std::setfill('0') << std::setw(8) << pc;
    return s.str();
}

O image(const std::string &data, uint32_t base = BASE) {
    return {{"data", hex(data)}, {"base", num(base)}, {"entry", num(base)}};
}
O image(const Words &w, uint32_t base = BASE) { return image(pack(w), base); }
O selection(A names, int cap = 8) { return {{"max_registers", cap}, {"functions", names}}; }
O main_spec(O main) { return {{"main", main}}; }
void put32(std::string &s, size_t at, uint32_t v) { s.replace(at, 4, pack({v})); }
std::pair<std::string, A> split(int count, uint32_t base = BASE) {
    Words words;
    A seeds;
    for (int i = 0; i < count; ++i) {
        words = words + Words{imm(9, 0, 2, i)} + RET;
        seeds.push_back(num(base + i * 12));
    }
    return {pack(words), seeds};
}
std::string jump(int entries, bool far = false) {
    uint32_t table = BASE + 0x400;
    Words w = {imm(15, 0, 8, (table + 0x8000) >> 16),
               imm(9, 8, 8, table & 65535),
               r(0, 0, 4, 9, 2),
               r(33, 8, 9, 8),
               imm(35, 8, 8, 0),
               0,
               r(8, 8),
               0};
    std::string data(std::max(0x800, 0x400 + (entries + 8) * 4), '\0');
    data.replace(0, pack(w).size(), pack(w));
    for (int i = 0; i < entries; ++i)
        put32(data, 0x400 + i * 4, far && i == entries - 1 ? BASE + 0x20000 : BASE + 32 + (i % 4) * 8);
    for (int i = 0; i < 4; ++i)
        data.replace(32 + i * 8, 8, pack(RET));
    return data;
}
uint32_t random_word(lh::PyRandom &rng, uint32_t base, int count) {
    double kind = rng.random();
    if (kind < 0.45)
        return rng.getrandbits(32);
    if (kind < 0.70) {
        if (rng.random() < 0.8) {
            auto op = rng.choice(std::vector<int>{4, 5, 6, 7, 1, 2, 3});
            auto rs = rng.randrange(32), rt = rng.randrange(32), val = rng.randrange(-16, 16);
            return imm(op, rs, rt, val);
        }
        auto op = rng.choice(std::vector<int>{2, 3});
        auto target = base + rng.randrange(count) * 4;
        return j(op, target);
    }
    if (kind < 0.85) {
        auto op = rng.choice(std::vector<int>{0,  2,  3,  4,  6,  7,  8,  9,  12, 13, 16, 17, 18, 19,
                                              24, 25, 26, 27, 32, 33, 34, 35, 36, 37, 38, 39, 42, 43});
        auto rs = rng.randrange(32), rt = rng.randrange(32), rd = rng.randrange(32), sh = rng.randrange(32);
        return r(op, rs, rt, rd, sh);
    }
    auto op = rng.choice(std::vector<int>{8,  9,  10, 11, 12, 13, 14, 15, 32, 33, 34, 35, 36,
                                          37, 38, 40, 41, 42, 43, 46, 50, 58, 16, 18, 49});
    auto rs = rng.randrange(32), rt = rng.randrange(32);
    auto val = rng.getrandbits(16);
    return imm(op, rs, rt, val);
}
lh::fs::path emit(lh::Case &t, const O &spec, const std::string &label) {
    auto path = t.temp() / (label + ".json"), out = t.temp() / (label + "-c");
    lh::write_file(path, J(spec).dump());
    t.ok(t.harness("lionheart-gen-spec").args({path, out}).timeout(120));
    return out;
}
void compare(lh::Case &t, const O &spec, const std::string &label, const lh::fs::path &out) {
    std::string key = t.name() + "/" + label;
    auto cmd = t.host("cmake").args({"-DKEY=" + key, "-DOUTPUT=" + out.string(),
                                     "-DREFERENCE=" + (t.root() / "tests/native/generator_reference.tsv").string()});
    if (const char *dir = std::getenv("LH_REWRITE_GENERATOR_REFERENCE")) {
        lh::fs::create_directories(dir);
        cmd.arg("-DREWRITE=" + (lh::fs::path(dir) / (lh::sha256(key) + ".tsv")).string());
    }
    t.ok(cmd.args({"-P", t.root() / "tests/native/generator_reference.cmake"}));
}
lh::fs::path parity(lh::Case &t, const O &spec, const std::string &label) {
    auto out = emit(t, spec, label);
    compare(t, spec, label, out);
    return out;
}
void spec_error(lh::Case &t, const O &spec, const std::string &message) {
    auto path = t.temp() / "invalid.json";
    lh::write_file(path, J(spec).dump());
    auto res = t.run(t.harness("lionheart-gen-spec").args({path, t.temp() / "invalid-output"}));
    LH_CHECK(res.rc > 0);
    LH_CONTAINS(res.err, message);
    LH_NOT_CONTAINS(res.err, "Sanitizer");
    LH_NOT_CONTAINS(res.err, "runtime error");
}
std::string source(const lh::fs::path &out) { return lh::read_file(out / "functions_000.c"); }
void contains_if(const std::string &s, const std::string &text, bool expected) {
    LH_EQ(s.find(text) != std::string::npos, expected);
}

void programs(lh::Case &t, const std::string &fn, const J &p) {
    auto timing = p.contains("timing") ? p["timing"].text() : "exact";
    if (fn == "test_recompiler_cases") {
        const auto &table = lh::recompiler::table();
        A seeds, overlays;
        for (const auto &name : table.order)
            if (name != "fallback_entry")
                seeds.push_back(num(table.pcs.at(name)));
        for (int index : {0, 1, 32, 64}) {
            auto blob = pack(Words{r(33, 31, 0, 25), j(3, table.pcs.at("callee")), 0, imm(9, 0, 2, 42 + index % 32),
                                   r(33, 25, 0, 31)} +
                             RET) +
                        std::string(16, '\0');
            auto row = image(blob, 0x80098000);
            row["index"] = index;
            overlays.push_back(row);
        }
        for (auto mode : {"fast", "trace"})
            for (auto host : {"precise", "fast"}) {
                if (timing != "exact" && std::string(mode) == "trace")
                    continue;
                auto main = image(table.data);
                main["seeds"] = seeds;
                O spec = {
                    {"main", main}, {"overlays", overlays}, {"mode", mode}, {"host_mode", host}, {"timing", timing}};
                if (p["cache"].boolean()) {
                    A names;
                    for (const auto &s : seeds)
                        names.push_back(symbol(s.integer()));
                    for (const auto &ov : overlays)
                        names.push_back(
                            symbol(ov["entry"].integer(), "ov" + std::to_string(ov["index"].integer()) + "_"));
                    spec["register_cache"] = selection(names);
                }
                parity(t, spec, std::string("tier1-") + mode + "-" + host);
            }
        return;
    }
    if (fn == "test_random_programs") {
        int seed = p["seed"].integer();
        lh::PyRandom rng(0x4c48 + seed);
        int count = rng.choice(std::vector<int>{64, 128, 256, 600});
        uint32_t base = BASE + rng.choice(std::vector<int>{0, 4, 8});
        Words words;
        for (int i = 0; i < count; ++i)
            words.push_back(random_word(rng, base, count));
        auto data = pack(words);
        if (rng.random() < 0.5) {
            auto start = rng.randrange(0, data.size() - 64);
            data.replace(start, 64, 64, '\0');
        }
        A seeds;
        int n = rng.randrange(1, 8);
        for (int i = 0; i < n; ++i)
            seeds.push_back(num(base + rng.randrange(count) * 4));
        int variant = rng.randrange(4);
        std::string mode = variant < 2 ? "fast" : "trace", host = variant % 2 ? "fast" : "precise";
        if (timing != "exact")
            mode = "fast";
        Words ow;
        for (int i = 0; i < count / 2; ++i)
            ow.push_back(random_word(rng, 0x80098000, count / 2));
        auto main = image(data, base);
        main["entry"] = num(base + rng.randrange(count) * 4);
        main["seeds"] = seeds;
        bool scan = rng.random() < 0.5;
        main["scan_calls"] = scan;
        auto overlay = image(ow, 0x80098000);
        overlay["index"] = rng.choice(std::vector<int>{0, 1, 27, 33, 70});
        overlay["seeds"] = A{num(0x80098000)};
        overlay["scan_calls"] = false;
        O spec = {{"mode", mode}, {"host_mode", host}, {"timing", timing}, {"main", main}, {"overlays", A{overlay}}};
        if (timing != "exact") {
            Image im(data, base, main["entry"].integer());
            std::vector<uint32_t> sv;
            for (auto &s : seeds)
                sv.push_back(s.integer());
            A names;
            for (auto pc : Discovery(im, sv, scan).entries())
                names.push_back(symbol(pc));
            if (names.empty())
                names.push_back("f_00000000");
            spec["register_cache"] = selection(names, 1 + seed % 8);
        }
        parity(t, spec, "random" + std::to_string(seed));
        return;
    }
    if (fn == "test_unit_split_boundaries" || fn == "test_duplicate_owners_across_sets") {
        int n = fn == "test_unit_split_boundaries" ? p["functions"].integer() : 30;
        auto [data, seeds] = split(n);
        auto main = image(data);
        main["seeds"] = seeds;
        main["scan_calls"] = false;
        A overlays;
        if (fn == "test_unit_split_boundaries") {
            auto [od, os] = split(n, 0x80098000);
            auto ov = image(od, 0x80098000);
            ov["index"] = 5;
            ov["seeds"] = os;
            ov["scan_calls"] = false;
            overlays.push_back(ov);
            parity(t, O{{"main", main}, {"overlays", overlays}}, "split" + std::to_string(n));
        } else {
            for (int index : {1, 2, 33, 34}) {
                auto blob = data;
                if (index == 33)
                    put32(blob, blob.size() - 4, 0x12345678);
                auto ov = image(blob, index == 34 ? BASE + 0x1000 : BASE);
                ov["index"] = index;
                A ss = seeds;
                if (index == 34)
                    for (auto &s : ss)
                        s = num(s.integer() + 0x1000);
                ov["seeds"] = ss;
                ov["scan_calls"] = false;
                overlays.push_back(ov);
            }
            for (auto mode : {"fast", "trace"})
                parity(t, O{{"main", main}, {"overlays", overlays}, {"mode", mode}}, std::string("dupes-") + mode);
        }
        return;
    }
    if (fn == "test_jump_tables") {
        int entries = p["entries"].integer();
        bool far = p["far"].boolean();
        auto main = image(jump(entries, far));
        main["seeds"] = A{num(BASE)};
        main["scan_calls"] = false;
        for (auto v :
             std::vector<std::pair<std::string, std::string>>{{"fast", "precise"}, {"fast", "fast"}, {"trace", "fast"}})
            parity(t, O{{"main", main}, {"mode", v.first}, {"host_mode", v.second}},
                   "jump-" + std::to_string(entries) + "-" + (far ? "True" : "False") + "-" + v.first + "-" + v.second);
        return;
    }
    if (fn == "test_poll_sites") {
        auto shape = p["shape"].text();
        uint32_t base = shape == "cross" ? 0x80010ff4 : BASE, getter = base + (shape == "cross" ? 0x200c : 0x100),
                 address = shape == "scratch" ? 0x1f800000
                           : shape == "mmio"  ? 0x1f801070
                                              : 0x80100004;
        Words w = {r(33, 31, 0, 16), j(3, getter), 0, imm(5, 2, 0, -3), 0, r(8, 16), 0};
        w.resize((getter - base) / 4);
        w = w + Words{imm(15, 0, 2, ((address + 0x8000) >> 16) & 65535), imm(35, 2, 2, address & 65535), r(8, 31),
                      shape == "side_effect" ? imm(43, 20, 9, 0) : 0};
        for (auto host : {"precise", "fast"})
            parity(t, O{{"main", image(w, base)}, {"host_mode", host}}, "poll-" + shape + "-" + host);
        return;
    }
    if (fn == "test_idle_site" || fn == "test_integer_edges") {
        O spec;
        if (fn == "test_idle_site")
            spec = main_spec(image(Words{imm(35, 20, 8, 0), 0, imm(9, 8, 8, -1), imm(43, 20, 8, 0), imm(35, 20, 8, 0),
                                         0, imm(5, 8, 9, 3), 0, r(8, 31), 0, imm(15, 0, 8, 0x8010), imm(35, 8, 8, 4), 0,
                                         r(42, 8, 10, 8), imm(5, 8, 0, -15), 0, r(8, 31), 0}));
        else {
            auto w = Words{imm(15, 0, 8, 0x8000), imm(9, 8, 8, -1), imm(4, 0, 0, 0x7fff), imm(9, 0, 9, 1)} +
                     Words(59, 0) + Words{imm(5, 8, 9, -2)};
            auto zero = image(Words{imm(4, 0, 0, 1), 0, imm(5, 8, 0, 0x7fff), j(3, 0), 0} + RET, 0);
            zero["index"] = 7;
            zero["seeds"] = A{0};
            zero["scan_calls"] = false;
            auto neg =
                image(Words{imm(5, 8, 0, -0x8000), imm(9, 8, 8, -0x8000), imm(13, 8, 8, 65535)} + RET, 0x7fff0000);
            neg["index"] = 99;
            neg["seeds"] = A{num(0x7fff0000)};
            neg["scan_calls"] = false;
            spec = {{"main", image(w, 0xffffff00)}, {"overlays", A{zero, neg}}};
        }
        for (auto v : std::vector<std::pair<std::string, std::string>>{
                 {"fast", "precise"}, {"fast", "fast"}, {"trace", "fast"}}) {
            spec["mode"] = v.first;
            spec["host_mode"] = v.second;
            parity(t, spec, (fn == "test_idle_site" ? "idle-" : "edges-") + v.first + "-" + v.second);
        }
        return;
    }
    if (fn == "test_unaligned_image_base") {
        auto main = image(std::string(2, '\0') + pack(Words{imm(9, 0, 2, 7)} + RET + Words{0, 0} + RET), BASE + 2);
        main["entry"] = num(BASE + 4);
        main["scan_calls"] = false;
        parity(t, main_spec(main), "unaligned");
        return;
    }
    if (fn == "test_empty_functions_and_code_ranges") {
        parity(t, main_spec(image(std::string(128, '\0'))), "empty");
        std::string data(512, '\0');
        data.replace(0, 16, pack(Words{j(3, BASE + 128), 0} + RET));
        put32(data, 256, j(3, BASE + 384));
        auto main = image(data);
        main["code_ranges"] = A{A{num(BASE), num(BASE + 128)}};
        parity(t, main_spec(main), "ranges");
        main = image(Words{imm(9, 0, 2, 77)} + RET + Words{0, 0} + RET);
        main["entry_points"] = A{num(BASE + 24)};
        main["scan_calls"] = false;
        parity(t, main_spec(main), "entry-points");
        return;
    }
    if (fn == "test_catalog_metadata_and_poll_policy") {
        auto [data, seeds] = split(5);
        A overlays;
        for (int index : {128, 129, 130, 27, 59, 33}) {
            auto ov = image(data);
            ov["index"] = index;
            ov["seeds"] = seeds;
            ov["scan_calls"] = false;
            if (index >= 128)
                ov["metadata"] = O{{"name", index == 128 ? "movie" : "other"},
                                   {"names", A{"movie", "café 🎬"}},
                                   {"discs", A{1, 3}},
                                   {"paths", A{"data/世界/a.bin", "data/\"quoted\"\\path\t.bin",
                                               "data/\x7f"
                                               "del"}},
                                   {"catalog_sha1", std::string(40, '0')},
                                   {"catalog_size", 1234},
                                   {"disc", index == 130 ? J(1) : J()},
                                   {"poll_forward", index == 129}};
            overlays.push_back(ov);
        }
        auto [md, ms] = split(2);
        auto main = image(md);
        main["seeds"] = ms;
        main["scan_calls"] = false;
        parity(t, O{{"main", main}, {"overlays", overlays}}, "metadata");
        return;
    }
    if (fn == "test_timing_regions_and_owners") {
        Words unit = {
            imm(4, 0, 0, 2), imm(43, 20, 8, 0), imm(9, 0, 9, 99), imm(9, 0, 9, 3), imm(35, 20, 8, 0), 0, r(8, 31), 0};
        Words words;
        A seeds;
        for (int n = 0; n < 50; ++n) {
            words = words + unit;
            seeds.push_back(num(BASE + n * 32));
        }
        A names = {symbol(BASE), symbol(BASE + 49 * 32), symbol(BASE, "ov2_"), "f_00000000"};
        auto main = image(words);
        main["entry_points"] = seeds;
        main["scan_calls"] = false;
        auto ov = main;
        ov["index"] = 2;
        auto cache = selection(names);
        cache["fuse_forward"] = p["fused"].boolean() ? names : A{};
        auto out =
            parity(t, O{{"timing", timing}, {"main", main}, {"overlays", A{ov}}, {"register_cache", cache}}, "regions");
        auto manifest = J::parse(lh::read_file(out / "manifest.json"));
        if (p["fused"].boolean())
            LH_CHECK(manifest["forward_regions"] == J(A{names[0], names[1]}));
        else
            LH_CHECK(!manifest.contains("forward_regions"));
        return;
    }
    if (fn == "test_timing_inline_and_region_bounds") {
        int size = p["size"].integer();
        auto names = A{symbol(BASE)};
        auto cache = selection(names);
        cache["fuse_forward"] = names;
        O spec = {
            {"timing", timing}, {"main", image(Words(size - 2, imm(9, 8, 8, 1)) + RET)}, {"register_cache", cache}};
        if (size == 513)
            spec_error(t, spec, "exceeds 512 instructions");
        else
            parity(t, spec, "bounded");
        spec["register_cache"] = selection(A{"f_00000000"});
        contains_if(source(parity(t, spec, "cold")), "#ifdef FF8_TIMING_INLINE", size <= 32);
        return;
    }
    if (fn == "test_cache_qualification") {
        std::vector<std::pair<std::string, bool>> shapes = {{pack(Words{imm(9, 8, 8, 1)} + RET), true},
                                                            {pack({r(8, 8), 0}), false},
                                                            {pack({r(9, 8, 0, 31), 0}), false},
                                                            {jump(3), true}};
        for (size_t i = 0; i < shapes.size(); ++i) {
            auto out = parity(
                t,
                O{{"timing", timing}, {"main", image(shapes[i].first)}, {"register_cache", selection(A{symbol(BASE)})}},
                "qualify" + std::to_string(i));
            contains_if(source(out), "#ifdef FF8_PLAY_REG_CACHE", shapes[i].second);
        }
        return;
    }
    if (fn == "test_wait_hint_and_memory_variants") {
        Words w = Words{imm(35, 20, 8, 0), 0, imm(5, 8, 0, -3), 0} + RET;
        for (int op : {32, 33, 34, 35, 36, 37, 38, 40, 41, 42, 43, 46, 50, 58})
            for (int reg : {0, 1, 7, 15, 23, 28, 31})
                w = w + Words{imm(op, 20, reg, 3)} + RET;
        A seeds = {num(BASE)}, names = {symbol(BASE)};
        for (size_t n = 6; n < w.size(); n += 3) {
            seeds.push_back(num(BASE + n * 4));
            names.push_back(symbol(BASE + n * 4));
        }
        auto main = image(w);
        main["seeds"] = seeds;
        main["scan_calls"] = false;
        contains_if(
            source(parity(t, O{{"timing", timing}, {"main", main}, {"register_cache", selection(names)}}, "memory")),
            "c_timing_idle(c)", timing == "loose");
        return;
    }
    if (fn == "test_lowering_register_limit_and_helper_order" || fn == "test_lowering_textual_matches") {
        std::string text;
        int cap = 8;
        if (fn == "test_lowering_textual_matches") {
            text = p["source"].text();
            if (text.find("xcode_valid") != std::string::npos) {
                spec_error(t, O{{"lower_source", text}, {"max_registers", 8}},
                           "unclassified register-cache helper: xcode_valid");
                return;
            }
        } else {
            cap = p["cap"].integer();
            text = "void f_80010000(cpu_t *c) {\n";
            bool first = true;
            for (int reg : {9, 8, 7, 6, 5, 4, 3, 2, 1}) {
                if (!first)
                    text += '\n';
                first = false;
                text += "c->r[" + std::to_string(reg) + "] += c->r[" + std::to_string(reg) + "];";
            }
            text += "\nc->r[9]++;\nc_timing_block(c, c_merge_value(c, 8), 0, &code_80010000);\nif (code_valid(c, "
                    "&code_80010000)) c_tick(c, 0);\ngte_write(c, 15, ram_read(c, c->r[8], 4), 0);\ncpu_stop(c, "
                    "\"quote: \\\" ( )\");\nreturn;\n}\n";
        }
        parity(t, O{{"lower_source", text}, {"max_registers", cap}}, "lower");
        return;
    }
    if (fn == "test_lowering_rejects_unclassified_or_unclosed_calls") {
        spec_error(t, O{{"lower_source", p["source"]}, {"max_registers", 8}}, p["message"].text());
        return;
    }
    if (fn == "test_invalid_cache_selections") {
        spec_error(t, O{{"main", image(std::string(8, '\0'))}, {"register_cache", p["selection"]}},
                   p["message"].text());
        return;
    }
    if (fn == "test_rerun_keeps_timestamps_and_removes_stale_units") {
        auto [data, seeds] = split(30);
        auto main = image(data);
        main["seeds"] = seeds;
        main["scan_calls"] = false;
        auto spec = main_spec(main);
        auto out = emit(t, spec, "rerun");
        lh::write_file(out / "functions_099.c", "stale");
        lh::write_file(out / "keepme.c", "not ours");
        std::map<std::string, lh::fs::file_time_type> before, after;
        for (auto &e : lh::fs::directory_iterator(out))
            if (e.path().filename() != "functions_099.c")
                before[e.path().filename().string()] = e.last_write_time();
        emit(t, spec, "rerun");
        LH_CHECK(!lh::fs::exists(out / "functions_099.c"));
        LH_CHECK(lh::fs::exists(out / "keepme.c"));
        for (auto &e : lh::fs::directory_iterator(out))
            after[e.path().filename().string()] = e.last_write_time();
        LH_CHECK(before == after);
        return;
    }
    LH_FAIL("unhandled program case: " + fn);
}

std::string valid_exe(const std::string &body = std::string(0x400, '\0')) {
    std::string s(0x800, '\0');
    s.replace(0, 8, "PS-X EXE");
    put32(s, 0x10, BASE);
    put32(s, 0x18, BASE);
    put32(s, 0x1c, body.size());
    return s + body;
}
std::string disc() {
    std::string s(32 * 2352, '\0');
    size_t d = 16 * 2352 + 24;
    s[d] = 1;
    s.replace(d + 1, 5, "CD001");
    s[d + 156] = 34;
    put32(s, d + 158, 18);
    put32(s, d + 166, 2048);
    s[d + 181] = 2;
    return s;
}
std::string directory_record(std::string name, uint32_t lba, uint32_t size) {
    std::string s(33 + name.size() + (1 - name.size() % 2), '\0');
    s[0] = char(s.size());
    put32(s, 2, lba);
    put32(s, 10, size);
    s[32] = char(name.size());
    s.replace(33, name.size(), name);
    return s;
}
std::string sha1(const std::string &s) {
    char out[41];
    lhg_sha1_hex(reinterpret_cast<const uint8_t *>(s.data()), s.size(), out);
    return out;
}
std::string synthetic_disc(lh::Case &t) {
    auto data = disc(), exe = valid_exe();
    auto put = [&](int sector, const std::string &s) {
        for (size_t i = 0; i < s.size(); i += 2048)
            data.replace((sector + i / 2048) * 2352 + 24, std::min<size_t>(2048, s.size() - i), s.substr(i, 2048));
    };
    put(18, directory_record("SLUS_008.92;1", 20, exe.size()) + directory_record("FF8DISC1.IMG;1", 24, 4096));
    put(20, exe);
    put(24, pack(Words(266, 1)));
    lh::write_file(t.temp() / "ff8.bin", data);
    lh::write_file(t.temp() / "ff8.cue", "FILE \"ff8.bin\" BINARY\n");
    return sha1(exe);
}
lh::fs::path catalog(lh::Case &t, const std::string &hash, A overlays, A locations) {
    auto dir = t.temp() / "catalog";
    lh::fs::create_directories(dir);
    lh::write_file(
        dir / "overlays.json",
        J(O{{"exe_sha1", hash},
            {"main_code_ranges", A{A{"0x80010000", "0x80010400"}}},
            {"discs", O{{"1", O{{"exe", "SLUS_008.92"}, {"image", "FF8DISC1.IMG"}, {"overlays", overlays}}}}}})
            .dump());
    lh::write_file(
        dir / "files-disc1.json",
        J(O{{"exe", O{{"name", "SLUS_008.92"}}}, {"image", O{{"lba", 24}, {"size", 4096}}}, {"entries", A{}}}).dump());
    lh::write_file(dir / "overlays-all.json", J(O{{"overlays", A{O{{"name", "x"},
                                                                   {"names", A{"x"}},
                                                                   {"paths", A{"x"}},
                                                                   {"sha1", std::string(40, '0')},
                                                                   {"size", 1},
                                                                   {"locations", locations}}}}})
                                                  .dump());
    return dir;
}
std::string cli(lh::Case &t, std::initializer_list<lh::Arg> args) {
    auto res = t.run(t.harness("lionheart-gen").args(args).cwd(t.temp()).timeout(120));
    LH_CHECK(res.rc > 0);
    LH_NOT_CONTAINS(res.err, "Sanitizer");
    LH_NOT_CONTAINS(res.err, "runtime error");
    LH_CHECK_MSG(res.err.find("lionheart-gen: ") == 0 || res.err.find("usage") != std::string::npos, res.err);
    return res.err;
}
void direct(lh::Case &t, const std::string &fn, const J &p) {
    if (fn == "test_native_forward_region_behavior") {
        bool fused = p["fused"].boolean(), cache = p["cache"].boolean();
        Program program{Image::words({imm(9, 0, 9, 1), imm(4, 0, 0, 2), imm(9, 0, 9, 2), imm(9, 0, 9, 99),
                                      imm(9, 0, 9, 3), imm(35, 20, 8, 0), 0, r(8, 31), 0},
                                     BASE)};
        program.timing = "block";
        program.register_cache = RegisterCache{8, {"f_80010000"}, std::vector<std::string>{}};
        if (fused)
            program.register_cache->fuse_forward->push_back("f_80010000");
        auto main = image(program.image.data);
        main["seeds"] = A{num(BASE)};
        auto policy = selection(A{"f_80010000"});
        policy["fuse_forward"] = fused ? A{"f_80010000"} : A{};
        O spec = {{"main", main}, {"timing", "block"}, {"host_mode", "fast"}, {"register_cache", policy}};
        auto outdir = emit(t, spec, "behavior");
        auto binary = t.temp() / "probe.so";
        auto cmd = t.host("cc").args({"-std=c99", "-O2", "-DFF8_COMPACT", "-DFF8_TIMING_INLINE"});
        if (cache)
            cmd.arg("-DFF8_PLAY_REG_CACHE");
        cmd.args({"-shared", "-fPIC", "-Iruntime", "-I" + outdir.string(), "runtime/cpu.c", "runtime/interp.c",
                  "runtime/timing.c", t.root() / "tests/native/cases/test_timing_regions.c"});
        for (const auto &entry : lh::fs::directory_iterator(outdir))
            if (entry.path().extension() == ".c")
                cmd.arg(entry.path());
        t.ok(cmd.args({"-o", binary}));
        void *handle = dlopen(binary.c_str(), RTLD_NOW | RTLD_LOCAL);
        LH_CHECK_MSG(handle != nullptr, "cannot load forward probe");
        lh::Library lib(handle, binary.string());
        auto check = lib.fn<void(const void *, unsigned, uint32_t *)>("check");
        for (unsigned deadline : {1000u, 4u}) {
            std::array<uint32_t, 4> out{};
            check(program.image.data.data(), deadline, out.data());
            auto expected = deadline == 1000 ? std::array<uint32_t, 4>{8, 0, 16, 3}
                            : fused          ? std::array<uint32_t, 4>{8, 8, 16, 77}
                                             : std::array<uint32_t, 4>{8, 6, 16, 3};
            for (size_t i = 0; i < 4; ++i)
                LH_EQ(out[i], expected[i]);
        }
        return;
    }
    if (fn == "test_timing_entries_and_public_cache_option") {
        auto data = pack(RET + Words{0, 0, imm(9, 8, 8, 1)} + RET), exe = valid_exe(data);
        auto ep = t.temp() / "program.exe", cp = t.temp() / "catalog";
        lh::write_file(ep, exe);
        lh::fs::create_directory(cp);
        std::ostringstream end;
        end << "0x" << std::hex << BASE + data.size();
        lh::write_file(cp / "overlays.json", J(O{{"exe_sha1", sha1(exe)},
                                                 {"main_code_ranges", A{A{"0x80010000", end.str()}}},
                                                 {"timing_entry_points", A{"0x80010010"}}})
                                                 .dump());
        auto main = image(data);
        main["code_ranges"] = A{A{num(BASE), num(BASE + data.size())}};
        main["timing_entry_points"] = A{num(BASE + 16)};
        auto timing = p["timing"].text();
        auto out = parity(t,
                          O{{"timing", timing},
                            {"main", main},
                            {"exe_path", ep.string()},
                            {"catalog_dir", cp.string()},
                            {"register_cache", selection(A{symbol(BASE + 16)}, 1)}},
                          "public-api");
        contains_if(source(out), "#ifdef FF8_PLAY_REG_CACHE", timing != "exact");
        return;
    }
    if (fn == "test_experimental_trace_rejected") {
        auto timing = p["timing"].text();
        spec_error(t, O{{"main", image(std::string(8, '\0'))}, {"timing", timing}, {"mode", "trace"}},
                   "experimental timing requires fast emission");
        LH_CONTAINS(cli(t, {"--mode", "trace", "--timing", timing, "unused.exe"}),
                    "experimental timing requires fast emission");
        return;
    }
    if (fn == "test_catalog_locations_are_bounded" || fn == "test_legacy_rows_are_bounded") {
        auto hash = synthetic_disc(t);
        A overlays, locations;
        if (fn == "test_catalog_locations_are_bounded")
            locations.push_back(O{{"disc", 1},
                                  {"path", "x"},
                                  {"sector", p["sector"]},
                                  {"size", p["size"]},
                                  {"read_size", 16},
                                  {"compressed", false},
                                  {"sha1_stored", std::string(40, '0')},
                                  {"loaded_sha1", std::string(40, '0')},
                                  {"loaded_size", 16},
                                  {"load_address", "0x80098000"}});
        else {
            O row = {
                {"compressed", false}, {"load_address", "0x80098000"}, {"size", 16}, {"sha1", std::string(40, '0')}};
            for (auto key : {"index", "img_sector", "stored_size"})
                row[key] = p["row"][key];
            overlays.push_back(row);
        }
        auto cp = catalog(t, hash, overlays, locations);
        auto err = fn == "test_catalog_locations_are_bounded"
                       ? cli(t, {"--disc", t.temp() / "ff8.cue", "--catalog", cp, "-o", t.temp() / "out"})
                       : cli(t, {"--disc", t.temp() / "ff8.cue", "--legacy-overlays", "--catalog", cp, "-o",
                                 t.temp() / "out"});
        LH_CONTAINS(err, p["message"].text());
        return;
    }
    if (fn == "test_malformed_inputs_end_with_a_message") {
        auto cp = t.root() / "games/ff8-usa", out = t.temp() / "out";
        auto write = [&](const char *name, const std::string &s) { lh::write_file(t.temp() / name, s); };
        auto run_disc = [&](const char *name) {
            return cli(t, {"--disc", t.temp() / name, "--catalog", cp, "-o", out});
        };
        auto run_exe = [&]() { return cli(t, {t.temp() / "wrong.exe", "--catalog", cp, "-o", out}); };
        auto data = disc();
        write("empty.bin", data);
        write("good.cue", "FILE \"empty.bin\" BINARY\n  TRACK 01 MODE2/2352\n");
        LH_CONTAINS(run_disc("good.cue"), "expected exactly one");
        LH_CONTAINS(cli(t, {"--disc", t.temp() / "good.cue", "--legacy-overlays", "--catalog", cp, "-o", out}),
                    "does not identify");
        write("nofile.cue", "REM nothing here\n");
        LH_CONTAINS(run_disc("nofile.cue"), "no FILE line");
        write("missing.cue", "FILE \"gone.bin\" BINARY\n");
        run_disc("missing.cue");
        write("truncated.bin", data.substr(0, 18 * 2352 + 10));
        write("truncated.cue", "FILE \"truncated.bin\" BINARY\n");
        LH_CONTAINS(run_disc("truncated.cue"), "truncated");
        put32(data, 16 * 2352 + 24 + 156 + 10, 0x7fffffff);
        write("huge.bin", data);
        write("huge.cue", "FILE \"huge.bin\" BINARY\n");
        LH_CONTAINS(run_disc("huge.cue"), "truncated");
        write("wrong.exe", "this is not a PlayStation executable");
        LH_CONTAINS(run_exe(), "not a PS-X EXE");
        write("wrong.exe", valid_exe());
        LH_CONTAINS(run_exe(), "unsupported EXE");
        auto exe = valid_exe();
        put32(exe, 0x1c, 0xfffffffc);
        write("wrong.exe", exe);
        LH_CONTAINS(run_exe(), "truncated EXE");
        write("wrong.exe", "");
        run_exe();
        write("wrong.exe", valid_exe());
        auto bad = t.temp() / "bad";
        lh::fs::create_directory(bad);
        cp = bad;
        LH_CONTAINS(run_exe(), "cannot open");
        for (const std::string &content :
             std::vector<std::string>{"{\"exe_sha1\": ", "[1, 2,, 3]", "{\"exe_sha1\": 12}", "{\"exe_sha1\": \"x\"}{}",
                                      "\"\\ud800\"", "{\"a\": 1e400000}", std::string(400, '[')}) {
            lh::write_file(bad / "overlays.json", content);
            run_exe();
        }
        lh::write_file(bad / "overlays.json", "{\"exe_sha1\": \"\xff\xfe\"}");
        run_exe();
        return;
    }
    programs(t, fn, p);
}
#include "generator_parity_rows.h"
struct Register {
    Register() {
        for (const auto &row : kParityRows) {
            std::string name = row.name;
            J params = J::parse(row.parameters);
            lh::add_case(__FILE__, name, lh::Tier::standard,
                         [name, params](lh::Case &t) { direct(t, name.substr(0, name.find('[')), params); });
        }
    }
} registration;
} // namespace
