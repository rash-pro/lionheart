// BIOS card operations share storage with SIO and complete through kernel events.
#include "lh_test.h"

static const char *const kCases[] = {"missing", "storage", "load", "device_directory", "unformatted", "callbacks", "failure", "pad", "filesystem", "filesystem_async", "filesystem_failure", "filesystem_corrupt"};

LH_TEST_P(test_bios_cards, kCases) {
    const auto path = t.temp() / "card.mcr";
    const std::string mode = p;
    if (mode == "unformatted") lh::write_file(path, std::string(131072, char(0xcc)));
    t.ok(t.harness("main").args({p, path}).timeout(30));
    if (mode == "storage" || mode == "unformatted") {
        const auto data = lh::read_file(path);
        if (mode == "storage") LH_EQ(data.size(), 131072u);
        LH_BYTES_EQ(data.substr(0, 2), "MC");
        std::string tail(128, char(0xcc));
        if (mode == "storage")
            for (int i = 0; i < 128; ++i) tail[i] = char(i ^ 0x5a);
        LH_CHECK(data.size() >= 128);
        LH_BYTES_EQ(data.substr(data.size() - 128), tail);
    }
}
