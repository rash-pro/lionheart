// Show the intentional deadline relaxation and preserved MMIO timestamps.
#include "generated.h"

#include <array>

using namespace lh::gen;

namespace {

const char *const kTiming[] = {"exact", "block", "loose", "block-inline", "loose-inline", "block-cache", "loose-cache"};

struct Timing {
    std::string mode, data;
    Compiled lib;
};

Timing timing(lh::Case &t, const std::string &variant) {
    Words words;
    for (unsigned n = 1; n < 5; ++n) words.push_back(imm(9, 0, 9, n));
    words = words + Words{imm(35, 20, 8, 0), 0, r(8, 31), 0};
    Program program{Image::words(words, 0x80010000)};
    program.timing = variant.substr(0, variant.find('-'));
    const bool cache = variant.find("-cache") != std::string::npos;
    if (cache) program.register_cache = RegisterCache{8, {"f_80010000"}, std::nullopt};
    Build build(t);
    build.program(program).probe("test_timing_boundaries.c")
        .flags({"-std=c99", "-O2", "-DFF8_COMPACT"}).runtime({"cpu.c", "interp.c", "timing.c"});
    if (variant.find('-') != std::string::npos)
        build.header_prefix(std::string("#define FF8_TIMING_INLINE 1\n") +
                            (cache ? "#define FF8_PLAY_REG_CACHE 1\n" : ""));
    return {program.timing, program.image.data, build.library()};
}

std::array<uint32_t, 5> check(Timing &fixture, unsigned address, unsigned deadline, unsigned fenced) {
    std::array<uint32_t, 5> out{};
    fixture.lib.fn<void(const void *, unsigned, unsigned, unsigned, uint32_t *)>("check")(
        fixture.data.data(), address, deadline, fenced, out.data());
    return out;
}

LH_TEST_P(test_ram_read_crosses_deadline_only_in_experiment, kTiming) {
    auto fixture = timing(t, p);
    const auto out = check(fixture, 0x80100000, 4, 0);
    LH_EQ(out[0], fixture.mode == "exact" ? 99u : 0u);
    LH_EQ(out[1], fixture.mode == "exact" ? 4u : fixture.mode == "block" ? 16u : 0u);
    LH_EQ(out[2], 16u);
}

LH_TEST_P(test_mmio_uses_static_instruction_offset, kTiming) {
    auto fixture = timing(t, p);
    const auto out = check(fixture, 0x1f801814, 1000, 0);
    LH_EQ(out[0], 8u);
    LH_EQ(out[2], 16u);
}

LH_TEST_P(test_media_ram_fence_flushes_static_prefix, kTiming) {
    auto fixture = timing(t, p);
    const auto out = check(fixture, 0x80100000, 1000, 1);
    LH_EQ(out[3], 8u);
    LH_EQ(out[2], 16u);
}

LH_TEST_P(test_event_updates_survive_cached_register_continuation, kTiming) {
    auto fixture = timing(t, p);
    const auto out = check(fixture, 0x80100000, 4, 0);
    LH_EQ(out[4], fixture.mode == "block" ? 77u : 4u);
}

} // namespace
