// Selected forward regions retain prefix timestamps and materialize at events.
#include "generated.h"

#include <array>

using namespace lh::gen;

namespace {

const uint32_t BASE = 0x80010000;
struct Forward { const char *id; bool fused, cache; };
const Forward kForward[] = {{"block", false, false}, {"block-cache", false, true},
                            {"region", true, false}, {"region-cache", true, true}};

Program selected(const Words &words, bool fused = true) {
    Program program{Image::words(words, BASE)};
    program.timing = "block";
    program.register_cache = RegisterCache{8, {"f_80010000"}, std::vector<std::string>{}};
    if (fused) program.register_cache->fuse_forward->push_back("f_80010000");
    return program;
}

std::array<uint32_t, 4> forward(lh::Case &t, const Forward &row, unsigned deadline) {
    Program program = selected({imm(9, 0, 9, 1), imm(4, 0, 0, 2), imm(9, 0, 9, 2),
                                imm(9, 0, 9, 99), imm(9, 0, 9, 3), imm(35, 20, 8, 0), 0, r(8, 31), 0}, row.fused);
    Build build(t);
    build.program(program).probe("test_timing_regions.c")
        .flags({"-std=c99", "-O2", "-DFF8_COMPACT", "-DFF8_TIMING_INLINE"})
        .runtime({"cpu.c", "interp.c", "timing.c"});
    if (row.cache) build.flag("-DFF8_PLAY_REG_CACHE");
    auto lib = build.library();
    std::array<uint32_t, 4> out{};
    lib.fn<void(const void *, unsigned, uint32_t *)>("check")(program.image.data.data(), deadline, out.data());
    return out;
}

void check_forward(const std::array<uint32_t, 4> &out, const std::array<uint32_t, 4> &expected) {
    for (size_t i = 0; i < out.size(); ++i) LH_EQ(out[i], expected[i]);
}

LH_TEST_P(test_mmio_flushes_the_complete_forward_prefix, kForward) {
    check_forward(forward(t, p, 1000), {8, 0, 16, 3});
}

LH_TEST_P(test_region_event_changes_survive_cached_continuation, kForward) {
    check_forward(forward(t, p, 4), p.fused ? std::array<uint32_t, 4>{8, 8, 16, 77} :
                                            std::array<uint32_t, 4>{8, 6, 16, 3});
}

struct Invalid { const char *id, *json; };
const Invalid kInvalid[] = {{"None", "null"}, {"f_80010000", "\"f_80010000\""}, {"regions2", "{}"},
                            {"regions3", "[[]]"}, {"regions4", "[{}]"},
                            {"regions5", "[\"f_80010000\", \"f_80010000\"]"}, {"regions6", "[\"f_80010004\"]"}};

LH_TEST_P(test_region_policy_rejects_invalid_or_unselected_owners, kInvalid) {
    const auto path = t.temp() / "policy.json";
    lh::write_file(path, std::string("{\"max_registers\":8,\"functions\":[\"f_80010000\"],\"fuse_forward\":") + p.json + "}");
    LH_CONTAINS(error_of([&] { read_selection(path); }), "fuse_forward");
}

LH_TEST(test_region_policy_retains_the_existing_selection) {
    const auto path = t.temp() / "policy.json";
    lh::write_file(path, "{\"max_registers\":8,\"functions\":[\"f_80010000\"],\"fuse_forward\":[\"f_80010000\"]}");
    const auto selection = read_selection(path);
    LH_EQ(selection.max_registers, 8);
    LH_EQ(selection.functions.size(), 1u);
    LH_EQ(selection.functions[0], "f_80010000");
    LH_CHECK(selection.fuse_forward.has_value());
    LH_EQ(selection.fuse_forward->size(), 1u);
    LH_EQ(selection.fuse_forward->at(0), "f_80010000");
}

void check_owners(const lh::fs::path &directory, const std::vector<std::string> &expected) {
    const auto owners = Json::file(directory / "manifest.json").at("forward_regions");
    LH_EQ(owners.size(), expected.size());
    for (size_t i = 0; i < expected.size(); ++i) LH_EQ(owners.at(i).str(), expected[i]);
}

LH_TEST(test_region_manifest_reports_only_emitted_owners) {
    auto program = selected({r(8, 31), 0});
    program.register_cache->functions.push_back("f_80010010");
    program.register_cache->fuse_forward->push_back("f_80010010");
    generate(t, program, t.temp());
    check_owners(t.temp(), {"f_80010000"});
}

LH_TEST(test_region_manifest_covers_every_translation_unit) {
    Words words;
    const Words unit = {imm(4, 0, 0, 2), 0, imm(9, 0, 9, 99), imm(9, 0, 9, 3), 0, r(8, 31), 0, 0};
    for (unsigned n = 0; n < 50; ++n) words = words + unit;
    auto program = selected(words);
    for (unsigned n = 0; n < 50; ++n) program.image.entry_points.push_back(BASE + n * 32);
    const std::vector<std::string> owners = {"f_80010000", "f_80010620"};
    program.register_cache->functions = owners;
    program.register_cache->fuse_forward = owners;
    generate(t, program, t.temp());
    check_owners(t.temp(), owners);
}

struct Store { std::string id; bool cache; unsigned opcode, offset, reg; };
std::vector<Store> stores() {
    std::vector<Store> out;
    const unsigned opcodes[] = {43, 42, 46, 58}, offsets[] = {0, 3, 0, 0}, registers[] = {8, 8, 8, 23};
    const char *names[] = {"sw", "swl", "swr", "swc2"};
    for (unsigned i = 0; i < 4; ++i)
        for (bool cache : {false, true})
            out.push_back({std::string(names[i]) + (cache ? "-True" : "-False"), cache, opcodes[i], offsets[i], registers[i]});
    return out;
}

LH_TEST_P(test_forward_delay_store_invalidates_before_its_successor, stores()) {
    auto program = selected({imm(4, 0, 0, 2), imm(p.opcode, 20, p.reg, p.offset), imm(9, 0, 9, 99),
                             imm(9, 0, 9, 1), r(8, 31), 0});
    Build build(t);
    build.program(program).probe("test_timing_regions_store.c")
        .flags({"-std=c99", "-O2", "-DFF8_COMPACT", "-DFF8_TIMING_INLINE"})
        .runtime({"cpu.c", "interp.c", "timing.c"});
    if (p.cache) build.flag("-DFF8_PLAY_REG_CACHE");
    auto lib = build.library();
    std::array<uint32_t, 2> out{};
    lib.fn<void(const void *, uint32_t, uint32_t *)>("check")(program.image.data.data(), imm(9, 0, 9, 77), out.data());
    LH_EQ(out[0], 77u);
    LH_EQ(out[1], 10u);
}

struct Size { const char *id; unsigned value; };
const Size kSizes[] = {{"512", 512}, {"513", 513}};

LH_TEST_P(test_forward_regions_have_a_bounded_guest_prefix, kSizes) {
    const auto program = selected(Words(p.value - 2, imm(9, 8, 8, 1)) + RET);
    if (p.value == 513) {
        LH_CONTAINS(error_of([&] { generate(t, program, t.temp()); }), "exceeds 512 instructions");
    } else {
        generate(t, program, t.temp());
        check_owners(t.temp(), {"f_80010000"});
    }
}

} // namespace
