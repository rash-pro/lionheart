// Static command fields must select direct APIs without a runtime decoder.
#include "generated.h"

using namespace lh::gen;

namespace {

LH_TEST(test_command_fields_and_ignored_bits) {
    const struct { uint32_t word; const char *expected; } cases[] = {
        {0x4A080030, "gte_rtpt(c, 12, 0);"},
        {0x4A000401, "gte_rtps(c, 0, 1);"},
        {0x4A0FF406, "gte_nclip(c);"},
        {0x4A0FF42D, "gte_avsz3(c);"},
        {0x4A0FF42E, "gte_avsz4(c);"},
        {0x4A0FE412, "gte_mvmva(c, 12, 1, 3, 3, 3);"},
        {0x4A000012, "gte_mvmva(c, 0, 0, 0, 0, 0);"},
        {0x4A00003A, "gte_cmd(c, 0x4a00003au);"},
    };
    for (const auto &row : cases) {
        const Effects effect = effects(row.word, 0x80010000);
        LH_EQ(effect.lines.size(), 2u);
        LH_TEXT_EQ(effect.lines[1], row.expected);
        LH_EQ(effect.dst, 0);
        LH_TEXT_EQ(effect.value, "0");
        LH_EQ(effect.load, 0);
        LH_TEXT_EQ(effect.loaded, "0");
    }
}

} // namespace
