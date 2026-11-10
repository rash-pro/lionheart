// A due IRQ retires the branch before its timed partial-store delay slot.
#include "generated.h"

#include <array>

using namespace lh::gen;

namespace {

struct Row { std::string id, mode, stop; unsigned opcode; bool cache; };

std::vector<Row> rows() {
    std::vector<Row> out;
    for (const std::string stop : {"irq", "missing-slot"})
        for (bool cache : {false, true})
            for (unsigned opcode : {42u, 46u})
                for (const std::string mode : {"block", "loose"})
                    out.push_back({stop + "-" + (cache ? "True" : "False") + "-" +
                                   (opcode == 42 ? "swl" : "swr") + "-" + mode, mode, stop, opcode, cache});
    return out;
}

LH_TEST_P(test_partial_store_irq_and_branch_stop_accounting, rows()) {
    Words words = {imm(4, 0, 0, 1), imm(p.opcode, 20, 8, 1), r(8, 31), 0};
    if (p.stop == "missing-slot") words = {imm(9, 0, 9, 1), 0, 0, imm(4, 0, 0, -1)};
    Program program{Image::words(words, 0x80010000)};
    program.timing = p.mode;
    if (p.cache) program.register_cache = RegisterCache{8, {"f_80010000"}, std::nullopt};
    std::vector<std::pair<std::string, std::string>> replacements;
    if (p.stop == "missing-slot") replacements.emplace_back("c->next_event = 2;", "c->next_event = 1000;");
    Build build(t);
    build.program(program).probe("test_timing_partial_irq.c", replacements)
        .flags({"-std=c99", "-O2", "-DFF8_COMPACT", "-DFF8_TIMING_INLINE"})
        .runtime({"cpu.c", "interp.c", "timing.c"});
    if (p.cache) build.flag("-DFF8_PLAY_REG_CACHE");
    auto lib = build.library();
    std::array<uint32_t, 3> out{};
    lib.fn<void(const void *, uint32_t *)>("check")(program.image.data.data(), out.data());
    LH_EQ(out[0], 0u);
    LH_EQ(out[1], p.stop == "irq" ? 2u : 8u);
    LH_EQ(out[2], 1u);
}

} // namespace
