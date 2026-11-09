// Polling must observe SPU IRQs and RAM materialized by deferred MDEC DMA.
#include "generated.h"

#include <array>
#include <vector>

using namespace lh::gen;

namespace {

struct Row { std::string id; bool direct, control; unsigned media; };

static std::vector<Row> rows() {
    static const char *const media_ids[] = {"spu-irq", "mdec-dma", "spu-dma"};
    std::vector<Row> out;
    for (bool direct : {false, true}) {
        for (unsigned media = 0; media < 3; ++media) {
            for (bool control : {false, true}) {
                out.push_back({std::string(direct ? "True" : "False") + "-" + media_ids[media] + "-" +
                               (control ? "True" : "False"), direct, control, media});
            }
        }
    }
    return out;
}

LH_TEST_P(test_poll_observes_real_device_calendars, rows()) {
    const uint32_t base = 0x80010000, getter = 0x80010100, handler = 0x80010200;
    Words words = {r(33, 31, 0, 16), j(3, getter), 0, imm(5, 2, 0, -3), 0, r(8, 16), 0};
    words = words + Words(64 - words.size(), 0);
    words = words + Words{imm(15, 0, 2, 0x8018), imm(35, 2, 2, 0), r(8, 31), 0};
    words = words + Words(128 - words.size(), 0);
    words = words + Words{imm(15, 0, 8, 0x8018), imm(43, 8, 0, 0), r(8, 31), 0};
    Image image = Image::words(words, base);
    image.entry_points = {handler};
    const std::string data = image.data;
    Compiled lib = Build(t)
        .program(Program{image})
        .probe("test_poll_calendars.c", {{"DATA_SIZE", std::to_string(data.size())}})
        .flags({"-std=c99", "-O2", "-DFF8_STATS", p.direct ? "-DFF8_COMPACT" : "-DFF8_COMPACT=1"})
        .runtime_all()
        .library();
    auto check = lib.fn<void(const void *, unsigned, unsigned, unsigned, unsigned, uint64_t *)>("check");
    uint64_t skipped = 0;
    for (unsigned phase = 0; phase < 32; ++phase) {
        std::array<uint64_t, 79> native{}, reference{};
        check(data.data(), 0, p.control, p.media, phase, native.data());
        check(data.data(), 1, p.control, p.media, phase, reference.data());
        for (size_t index = 0; index < 77; ++index) {
            LH_CHECK_MSG(native[index] == reference[index], "control=" + std::to_string(p.control) + " media=" +
                         std::to_string(p.media) + " phase=" + std::to_string(phase) + " word=" + std::to_string(index));
        }
        LH_EQ(native[36], 0u);
        LH_CHECK(native[37] < 100000);
        if (p.media == 1) {
            LH_CHECK(native[43] > 0);
        } else if (p.media == 2) {
            LH_EQ(native[78], 0x180004u);
            LH_EQ(reference[78], 0x180004u);
        } else {
            LH_EQ(native[39], native[40]);
            LH_EQ(native[39], 1u);
        }
        skipped += native[77];
    }
    if (p.media != 1) LH_CHECK(skipped > 0);
}

} // namespace
