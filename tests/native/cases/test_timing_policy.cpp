// Experimental policies retain CPU semantics away from device deadlines.
#include "recompiler_fixture.h"

using namespace lh::gen;
using namespace lh::recompiler;

namespace {

const char *const kRelaxed[] = {"block", "loose", "block-inline", "loose-inline", "block-cache", "loose-cache"};

std::map<std::string, std::string> files_at(const lh::fs::path &directory) {
    std::map<std::string, std::string> out;
    for (const auto &file : lh::fs::directory_iterator(directory))
        out.emplace(file.path().filename().string(), lh::read_file(file.path()));
    return out;
}

LH_TEST(test_exact_is_default) {
    Program program{Image::words({imm(9, 0, 8, 1), r(8, 31), 0}, BASE)};
    generate(t, program, t.temp() / "default");
    program.timing = "exact";
    generate(t, program, t.temp() / "exact");
    const auto defaults = files_at(t.temp() / "default"), explicit_exact = files_at(t.temp() / "exact");
    LH_EQ(defaults.size(), explicit_exact.size());
    for (const auto &file : defaults) {
        LH_CHECK_MSG(explicit_exact.count(file.first), "missing exact output " + file.first);
        LH_BYTES_EQ(file.second, explicit_exact.at(file.first));
    }
    LH_NOT_CONTAINS(defaults.at("generated.h"), "timing.h");
}

LH_TEST(test_invalid_policy) {
    Program program{Image(std::string(8, '\0'), BASE, BASE)};
    program.discover_functions = false;
    program.timing = "invalid";
    LH_CONTAINS(error_of([&] { generate(t, program, t.temp()); }), "timing policy");
    program.timing = "block";
    program.mode = "trace";
    LH_CONTAINS(error_of([&] { generate(t, program, t.temp()); }), "requires fast");
}

LH_TEST(test_timing_entries_do_not_change_exact_discovery) {
    Image image = Image::words({r(8, 31), 0, 0, 0, imm(9, 0, 2, 42), r(8, 31), 0}, BASE);
    image.timing_entry_points = {BASE + 16};
    LH_CHECK(!Discovery(image).entries().count(BASE + 16));
    for (const std::string mode : {"block", "loose"}) {
        Program program{image};
        program.timing = mode;
        program.seeds = image.timing_entry_points;
        LH_CHECK(Discovery(image, program.seeds).entries().count(BASE + 16));
        generate(t, program, t.temp() / mode);
        const auto entries = Json::file(t.temp() / mode / "manifest.json").at("timing_entry_points");
        LH_EQ(entries.size(), 1u);
        LH_EQ(entries.at(0).str(), "0x80010010");
    }
}

const CaseTable &relaxed_table() {
    static const CaseTable built = [] {
        CaseTable tables = table();
        for (const std::string store : {"swl", "swr"})
            for (unsigned offset = 0; offset < 4; ++offset) {
                const auto name = "timing_self_modify_" + store + "_" + std::to_string(offset);
                const uint32_t pc = BASE + static_cast<uint32_t>(tables.order.size()) * 0x200;
                tables.set(name, Words{0, 0, imm(store == "swl" ? 42 : 46, 20, 8, offset), imm(9, 0, 9, 1)} + RET);
                tables.pcs[name] = pc;
                tables.data.resize(tables.order.size() * 0x200, '\0');
                const auto bytes = pack(tables.cases.at(name));
                tables.data.replace(pc - BASE, bytes.size(), bytes);
            }
        return tables;
    }();
    return built;
}

Compiled relaxed(lh::Case &t, const std::string &variant) {
    return compiled_library(t, "compact", relaxed_table(), [&](Program &program, Build &build) {
        program.timing = variant.substr(0, variant.find('-'));
        const bool cache = variant.find("-cache") != std::string::npos;
        if (cache) {
            RegisterCache selection;
            for (uint32_t pc : Discovery(program.image, program.seeds).entries()) {
                char name[16];
                std::snprintf(name, sizeof name, "f_%08x", pc);
                selection.functions.emplace_back(name);
            }
            program.register_cache = std::move(selection);
        }
        if (variant.find('-') != std::string::npos)
            build.header_prefix(std::string("#define FF8_TIMING_INLINE 1\n") +
                                (cache ? "#define FF8_PLAY_REG_CACHE 1\n" : ""));
    });
}

Outcome compare(lh::Case &t, const std::string &variant, const std::string &name,
                const Pairs &registers = {}, const Pairs &memory = {}) {
    auto lib = relaxed(t, variant);
    return diff(t, functions_of(lib), name, registers, memory, 0, relaxed_table());
}

struct Semantics { std::string id, variant, name; };
std::vector<Semantics> semantics() {
    std::vector<Semantics> out;
    for (const char *variant : kRelaxed)
        for (const char *name : {"load_delay", "load_overwrite", "load_twice", "load_zero",
                                "branch_delay", "load_branch", "load_in_slot", "return_load",
                                "load_slot_taken", "load_slot_fallthrough", "load_slot_twice",
                                "jr_capture", "memory", "gte", "call_direct", "call_indirect",
                                "call_return_load", "incoming_load_twice", "call_alt_link", "tail_jump",
                                "fallback_entry", "jump_table", "unaligned_0", "unaligned_1",
                                "unaligned_2", "unaligned_3", "swl_0", "swl_1", "swl_2", "swl_3",
                                "swr_0", "swr_1", "swr_2", "swr_3"})
            out.push_back({std::string(variant) + "-" + name, variant, name});
    return out;
}

LH_TEST_P(test_cpu_semantics, semantics()) { compare(t, p.variant, p.name); }

struct Alias { std::string id, variant; uint32_t base; };
std::vector<Alias> aliases() {
    std::vector<Alias> out;
    for (const char *variant : kRelaxed)
        for (uint32_t base : {0x80100000u, 0xa0100000u, 0x1f800000u, 0x9f800000u, 0xbf800000u})
            out.push_back({std::string(variant) + "-" + std::to_string(base), variant, base});
    return out;
}

LH_TEST_P(test_memory_aliases_and_scratch, aliases()) {
    compare(t, p.variant, "memory", {{20, p.base}}, {{p.base, 0x44332211}, {p.base + 4, 0x88776655}});
}

LH_TEST_P(test_watched_store_invalidates, kRelaxed) {
    const auto state = compare(t, p, "self_modify", {{20, relaxed_table().pcs.at("self_modify") + 4},
                                                    {8, imm(9, 0, 9, 77)}});
    LH_EQ(state.state[9], 77u);
}

struct Merged { std::string id, variant, store; unsigned offset; };
std::vector<Merged> merged() {
    std::vector<Merged> out;
    for (const char *variant : kRelaxed)
        for (unsigned offset = 0; offset < 4; ++offset)
            for (const std::string store : {"swl", "swr"})
                out.push_back({std::string(variant) + "-" + std::to_string(offset) + "-" + store, variant, store, offset});
    return out;
}

LH_TEST_P(test_merged_store_invalidates_before_the_next_instruction, merged()) {
    const auto name = "timing_self_modify_" + p.store + "_" + std::to_string(p.offset);
    const uint32_t replacement = p.store == "swl" ? imm(9, 0, 9, 77) : imm(15, 0, 9, 1);
    const uint32_t value = p.store == "swl" ? replacement << ((3 - p.offset) * 8) : replacement >> (p.offset * 8);
    const auto state = compare(t, p.variant, name, {{20, relaxed_table().pcs.at(name) + 12}, {8, value}});
    LH_EQ(state.state[9], p.store == "swl" ? 77u : 65536u);
}

LH_TEST(test_wait_hint_is_experimental) {
    Program program{Image::words({imm(35, 20, 8, 0), 0, imm(5, 8, 0, -3), 0}, BASE)};
    for (const std::string mode : {"loose", "block"}) {
        program.timing = mode;
        const auto directory = t.temp() / mode;
        generate(t, program, directory);
        std::string source;
        for (const auto &file : lh::fs::directory_iterator(directory))
            if (file.path().extension() == ".c") source += lh::read_file(file.path());
        // Emitting the idle call also proves the loop was classified as a wait.
        if (mode == "loose") LH_CONTAINS(source, "c_timing_idle(c)");
        else LH_NOT_CONTAINS(source, "c_timing_idle(c)");
    }
}

} // namespace
