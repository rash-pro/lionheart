// Scratchpad widths, aliases, load delays and event boundaries against the interpreter.
#include "generated.h"

#include <array>
#include <sstream>

using namespace lh::gen;

namespace {

static const char *const kVariants[] = {"compact", "helper", "fast", "precise"};

LH_TEST_P(test_scratchpad_matches_every_event_boundary, kVariants) {
    const std::string variant = p;
    const Words words = {imm(43, 20, 9, 0), imm(33, 20, 10, 2), imm(32, 20, 11, 1),
                         imm(37, 20, 12, 0), imm(36, 20, 13, 3), imm(35, 20, 14, 0),
                         imm(34, 20, 15, 3), imm(38, 20, 15, 0), imm(50, 20, 6, 0),
                         imm(58, 20, 6, 4), r(8, 31), 0};
    Program program{Image::words(words, 0x80010000)};
    program.host_mode = variant == "precise" ? "precise" : "fast";
    Build build(t);
    build.program(program).probe("test_scratchpad.c")
        .flags({"-std=c99", "-O2", "-DFF8_STATS"}).runtime({"cpu.c", "interp.c"});
    if (variant == "compact") build.flag("-DFF8_COMPACT");
    if (variant == "helper") build.flag("-DFF8_COMPACT=1");
    Compiled lib = build.library();
    const std::string data = program.image.data;
    auto check = lib.fn<void(const void *, unsigned, unsigned, unsigned, unsigned, uint32_t *)>("check");

    for (uint32_t alias : {0x1f8003f8u, 0x9f8003f8u, 0xbf8003f8u, 0x1f8003f9u, 0x1f8003fcu, 0x1f800400u}) {
        for (uint32_t sr : {0u, 0x401u, 0x10401u}) {
            for (uint32_t deadline = 1; deadline < 30; ++deadline) {
                std::array<uint32_t, 359> a{}, b{};
                check(data.data(), 0, deadline, alias, sr, a.data());
                check(data.data(), 1, deadline, alias, sr, b.data());
                std::ostringstream where;
                where << std::hex << "alias=" << alias << " sr=" << sr << std::dec << " deadline=" << deadline;
                LH_CHECK_MSG(a == b, where.str());
            }
        }
    }
}

} // namespace
