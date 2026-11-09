// Pure getter polling, including code in another page and IRQ-owned flags.
#include "generated.h"

#include <array>
#include <cstdio>

using namespace lh::gen;

namespace {

struct Polling { const char *id; const char *mode, *shape; };
static const Polling kPolling[] = {
    {"polling0", "compact", "ram"}, {"polling1", "helper", "ram"}, {"polling2", "fast", "ram"},
    {"polling3", "compact", "scratch"}, {"polling4", "helper", "cross"},
    {"polling5", "compact", "side_effect"}, {"polling6", "compact", "mmio"}, {"polling7", "nostats", "ram"},
};

static std::string py_hex(uint32_t value) {
    char text[16];
    std::snprintf(text, sizeof text, "0x%x", value);
    return text;
}

struct PollFixture { Compiled lib; std::string data; std::string shape, mode; };

static PollFixture polling(lh::Case &t, const Polling &param) {
    const std::string mode = param.mode, shape = param.shape;
    const uint32_t base = shape == "cross" ? 0x80010ff4 : 0x80010000;
    const uint32_t head = base + 4; (void)head;
    const uint32_t getter = base + (shape == "cross" ? 0x200c : 0x100);
    const uint32_t address = shape == "scratch" ? 0x1f800000 : shape == "mmio" ? 0x1f801070 : 0x80100004;
    const uint32_t low = address & 0xffff;
    const uint32_t high = ((address + 0x8000) >> 16) & 0xffff;
    Words words = {r(33, 31, 0, 16), j(3, getter), 0, imm(5, 2, 0, -3), 0, r(8, 16), 0};
    words = words + Words((getter - base) / 4 - words.size(), 0);
    words = words + Words{imm(15, 0, 2, high), imm(35, 2, 2, low), r(8, 31),
                          shape == "side_effect" ? imm(43, 20, 9, 0) : 0u};
    Program program{Image::words(words, base)};
    const std::string data = program.image.data;
    Build build(t);
    build.program(program)
        .probe("test_poll.c", {{"FLAG_ADDRESS", py_hex(address) + "u"},
                               {"DATA_SIZE", std::to_string(data.size())},
                               {"BASE", py_hex(base) + "u"},
                               {"GETTER", py_hex(getter) + "u"},
                               {"NATIVE_ABSENT", shape == "cross" ? "1" : "0"}})
        .flags({"-std=c99", "-O2"});
    if (mode != "nostats") build.flag("-DFF8_STATS");
    if (mode == "compact" || mode == "nostats") build.flag("-DFF8_COMPACT");
    if (mode == "helper") build.flag("-DFF8_COMPACT=1");
    build.runtime({"cpu.c", "interp.c"});
    return {build.library(), data, shape, mode};
}

using Check = uint64_t(const void *, unsigned, unsigned, unsigned, uint64_t *);

LH_TEST_P(test_poll_matches_every_event_boundary, kPolling) {
    PollFixture fixture = polling(t, p);
    auto check = fixture.lib.fn<Check>("check");
    uint64_t accelerated = 0;
    for (unsigned deadline = 1; deadline < 401; ++deadline) {
        std::array<uint64_t, 75> a{}, b{};
        accelerated += check(fixture.data.data(), 0, deadline, 0, a.data());
        check(fixture.data.data(), 1, deadline, 0, b.data());
        LH_CHECK_MSG(a == b, fixture.shape + " deadline=" + std::to_string(deadline));
    }
    const bool accelerates = fixture.shape != "side_effect" && fixture.shape != "mmio" && fixture.mode != "nostats";
    LH_EQ(accelerated > 0, accelerates);
}

LH_TEST_P(test_poll_rejects_isolation_fences_and_replaced_getters, kPolling) {
    PollFixture fixture = polling(t, p);
    auto check = fixture.lib.fn<Check>("check");
    for (unsigned control : {1u, 2u, 3u, 4u}) {
        for (unsigned deadline : {31u, 69u, 129u, 299u, 401u}) {
            std::array<uint64_t, 75> a{}, b{};
            const uint64_t accelerated = check(fixture.data.data(), 0, deadline, control, a.data());
            check(fixture.data.data(), 1, deadline, control, b.data());
            LH_CHECK_MSG(a == b, fixture.shape + " control=" + std::to_string(control) +
                                     " deadline=" + std::to_string(deadline));
            if (control != 4) LH_EQ(accelerated, 0u);
        }
    }
}

struct DirectRow { const char *id; bool direct; };
static const DirectRow kDirect[] = {{"False", false}, {"True", true}};

LH_TEST_P(test_poll_flag_cleared_by_real_irq_handler, kDirect) {
    const uint32_t base = 0x80010000, getter = 0x80010100, handler = 0x80010200;
    Words words = {r(33, 31, 0, 16), j(3, getter), 0, imm(5, 2, 0, -3), 0, r(8, 16), 0};
    words = words + Words(64 - words.size(), 0);
    words = words + Words{imm(15, 0, 2, 0x8010), imm(35, 2, 2, 4), r(8, 31), 0};
    words = words + Words(128 - words.size(), 0);
    words = words + Words{imm(15, 0, 8, 0x8010), imm(43, 8, 0, 4), r(8, 31), 0};
    Image image = Image::words(words, base);
    image.entry_points = {handler};
    const std::string data = image.data;
    Compiled lib = Build(t)
        .program(Program{image})
        .probe("test_poll_irq.c", {{"DATA_SIZE", std::to_string(data.size())}})
        .flags({"-std=c99", "-O2", "-DFF8_STATS", p.direct ? "-DFF8_COMPACT" : "-DFF8_COMPACT=1"})
        .runtime_all()
        .library();
    auto check = lib.fn<void(const void *, unsigned, unsigned, uint64_t *)>("check");
    for (unsigned target = 9990; target < 10020; ++target) {
        std::array<uint64_t, 43> a{}, b{};
        check(data.data(), 0, target, a.data());
        check(data.data(), 1, target, b.data());
        for (size_t index = 0; index < 41; ++index) {
            LH_CHECK_MSG(a[index] == b[index], "target=" + std::to_string(target) + " word=" + std::to_string(index));
        }
        LH_EQ(a[36], 0u);
        LH_EQ(a[39], 1u);
        LH_CHECK(a[40] >= 8);
        LH_CHECK_MSG(a[41] < 64 && a[42] > 4000, "target=" + std::to_string(target));
    }
}

} // namespace
