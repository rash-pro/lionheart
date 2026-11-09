// Differential countdown batching across every instruction boundary of an IRQ.
#include "generated.h"

#include <array>
#include <sstream>

using namespace lh::gen;

namespace {

using Check = uint64_t(const void *, unsigned, unsigned, unsigned, unsigned, uint32_t *);

static const char *const kIdleVariants[] = {"compact", "fast"};

static Words idle_words() {
    // Countdown in RAM; break on timeout, otherwise compare an IRQ-owned clock.
    return {imm(35, 20, 8, 0), 0, imm(9, 8, 8, -1), imm(43, 20, 8, 0),
            imm(35, 20, 8, 0), 0, imm(5, 8, 9, 3), 0, r(8, 31), 0,
            imm(15, 0, 8, 0x8010), imm(35, 8, 8, 4), 0, r(42, 8, 10, 8),
            imm(5, 8, 0, -15), 0, r(8, 31), 0};
}

static Compiled idle_library(lh::Case &t, const std::string &variant, std::string &data) {
    Program program{Image::words(idle_words(), 0x80010000)};
    data = program.image.data;
    Build build(t);
    build.program(program).probe("test_idle.c").flags({"-std=c99", "-O2", "-DFF8_STATS"}).runtime({"cpu.c", "interp.c"});
    if (variant == "compact") build.flag("-DFF8_COMPACT");
    return build.library();
}

LH_TEST_P(test_idle_matches_every_irq_boundary, kIdleVariants) {
    std::string data;
    Compiled lib = idle_library(t, p, data);
    auto check = lib.fn<Check>("check");
    bool saw_batch = false;
    for (unsigned counter : {0u, 1u, 3u, 10000u}) {
        for (unsigned deadline = 1; deadline < 201; ++deadline) {
            std::array<uint32_t, 40> a{}, b{};
            const uint64_t batch = check(data.data(), 0, deadline, counter, 0, a.data());
            check(data.data(), 1, deadline, counter, 0, b.data());
            std::ostringstream where;
            where << "counter=" << counter << " deadline=" << deadline;
            LH_CHECK_MSG(a == b, where.str());
            saw_batch |= batch > 0;
        }
    }
    LH_CHECK(saw_batch);
}

LH_TEST_P(test_idle_rejects_counter_clock_alias, kIdleVariants) {
    std::string data;
    Compiled lib = idle_library(t, p, data);
    auto check = lib.fn<Check>("check");
    std::array<uint32_t, 40> a{}, b{};
    LH_EQ(check(data.data(), 0, 151, 1000, 1, a.data()), 0u);
    check(data.data(), 1, 151, 1000, 1, b.data());
    LH_CHECK(a == b);
}

struct DirectRow { const char *id; bool direct; };
static const DirectRow kDirect[] = {{"False", false}, {"True", true}};

LH_TEST_P(test_idle_stays_compiled_across_masked_events, kDirect) {
    // A real timer IRQ calls compiled guest code to update the loop's clock.
    // Masked SPU samples and HBlank timer clocks precede that IRQ many times.
    const uint32_t base = 0x80010000;
    Words words = {imm(35, 20, 8, 0), 0, imm(9, 8, 8, -1), imm(43, 20, 8, 0),
                   imm(35, 20, 8, 0), 0, imm(5, 8, 9, 3), 0, r(8, 31), 0,
                   imm(15, 0, 8, 0x8010), imm(35, 8, 8, 4), 0, r(42, 8, 10, 8),
                   imm(5, 8, 0, -15), 0, r(8, 31), 0};
    words = words + Words(64 - words.size(), 0);
    words = words + Words{imm(15, 0, 8, 0x8010), imm(9, 0, 9, 1), imm(43, 8, 9, 4), r(8, 31), 0};
    Image image = Image::words(words, base);
    image.entry_points = {base + 256};
    const std::string data = image.data;
    Compiled lib = Build(t)
        .program(Program{image})
        .probe("test_idle_masked.c", {{"DATA_SIZE", std::to_string(data.size())}})
        .flags({"-std=c99", "-O2", "-DFF8_STATS", p.direct ? "-DFF8_COMPACT" : "-DFF8_COMPACT=1"})
        .runtime_all()
        .library();
    auto check = lib.fn<void(const void *, unsigned, unsigned, uint64_t *)>("check");
    for (unsigned target = 9990; target < 10020; ++target) {
        std::array<uint64_t, 44> a{}, b{};
        check(data.data(), 0, target, a.data());
        check(data.data(), 1, target, b.data());
        const std::array<uint64_t, 40> left = {
            a[0],a[1],a[2],a[3],a[4],a[5],a[6],a[7],a[8],a[9],a[10],a[11],a[12],a[13],a[14],a[15],
            a[16],a[17],a[18],a[19],a[20],a[21],a[22],a[23],a[24],a[25],a[26],a[27],a[28],a[29],a[30],a[31],
            a[32],a[33],a[34],a[35],a[36],a[37],a[38],a[39]};
        const std::array<uint64_t, 40> right = {
            b[0],b[1],b[2],b[3],b[4],b[5],b[6],b[7],b[8],b[9],b[10],b[11],b[12],b[13],b[14],b[15],
            b[16],b[17],b[18],b[19],b[20],b[21],b[22],b[23],b[24],b[25],b[26],b[27],b[28],b[29],b[30],b[31],
            b[32],b[33],b[34],b[35],b[36],b[37],b[38],b[39]};
        LH_CHECK_MSG(left == right, "target=" + std::to_string(target));
        LH_EQ(a[37], 1u);
        LH_EQ(a[42], 1u);
        LH_CHECK_MSG(a[43] >= 8, "target=" + std::to_string(target));
        LH_CHECK_MSG(a[40] < 32 && a[41] > 4000, "target=" + std::to_string(target));
    }
}

} // namespace
