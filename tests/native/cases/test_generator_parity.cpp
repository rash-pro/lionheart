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
    if (fn == "test_set_entry_points") {
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
    if (fn == "test_register_jumps_make_no_leaders") {
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
    if (fn == "test_hinted_zero_functions") {
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
    if (fn == "test_code_prefixes") {
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
