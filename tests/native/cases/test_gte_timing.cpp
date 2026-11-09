// GTE instructions retain the interpreter's cycle and hardware event boundaries.
#include "generated.h"
#include <array>
#include <set>
using namespace lh::gen;
namespace {
const char *const modes[] = {"compact", "compact-nostats", "fast", "precise"};
}
LH_TEST_P(test_commands_and_transfers_at_every_deadline, modes) {
    Words words = {cop(18,6,8,31), cop(18,4,8,30), cop(18,0,9,31),
        r(33,9,0,10), r(33,9,0,11), imm(50,20,0,0), imm(50,20,1,4),
        0x4A080001, 0x4A080030, 0x4A0FE412, imm(4,0,0,1), 0x4A000006,
        cop(18,4,8,15), cop(18,0,9,15), cop(18,2,12,31), imm(58,20,14,8),
        r(33,12,0,13), r(8,31), 0};
    auto data = pack(words);
    Program program{Image(data, 0x80010000, 0x80010000)};
    program.host_mode = std::string(p) == "precise" ? "precise" : "fast";
    Build build(t);
    build.program(program).probe("test_gte_timing.c", {{"DATA_BYTES", std::to_string(data.size())}})
        .flags({"-std=c99", "-O2", "-DFF8_DIRECT_RAM"}).runtime({"cpu.c", "interp.c", "gte.c"});
    if (std::string(p).find("compact") == 0) build.flag("-DFF8_COMPACT");
    if (std::string(p) != "compact-nostats") build.flag("-DFF8_STATS");
    auto lib = build.library();
    auto check = lib.fn<void(const void *, unsigned, unsigned, uint64_t *)>("check");
    std::set<uint64_t> observed;
    for (unsigned deadline = 1; deadline < 2 * words.size() + 5; ++deadline) {
        std::array<uint64_t,104> native{}, reference{};
        check(data.data(), 0, deadline, native.data());
        check(data.data(), 1, deadline, reference.data());
        for (size_t i=0; i<native.size(); ++i)
            LH_CHECK_MSG(native[i] == reference[i], "deadline " + std::to_string(deadline) + ", slot " + std::to_string(i) + ": " + std::to_string(native[i]) + " != " + std::to_string(reference[i]));
        LH_EQ(native[96], 2 * words.size());
        LH_EQ(native[97], 4u);
        observed.insert(native[102]);
    }
    LH_CHECK(observed == std::set<uint64_t>({0,1}));
}
