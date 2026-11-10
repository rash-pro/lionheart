// Live card ownership survives load; snapshot mode preserves deterministic isolation.
#include "host_fixtures.h"
struct Row { std::string id, mode, name; };
static std::vector<Row> rows() {
    std::vector<Row> out;
    for (auto name : {"equal", "changed", "absent", "failed", "wire", "bios-write", "bios-load", "bios-ack", "bios-file"})
        for (auto mode : {"live", "snapshot"}) out.push_back({std::string(name)+"-"+mode, mode, name});
    return out;
}
LH_TEST_P(test_restored_cards_and_guest_save, rows()) {
    auto r = t.ok(lh::dummy(t.harness("cards")).args({p.name, p.mode, t.temp()}).timeout(60));
    LH_CONTAINS(r.out, "Cards:");
    int warnings = 0;
    for (const auto &line : lh::lines(r.err)) if (line.rfind("Warning: live memory card", 0) == 0) ++warnings;
    LH_EQ(warnings, (p.mode == "live" && p.name != "equal" && p.name != "failed" ? 2 : 0));
    auto restored = lh::files_starting(t.temp(), "restored-card");
    if (p.mode == "live") { LH_NOT_CONTAINS(r.out, "Restored card"); LH_CHECK(restored.empty()); }
    else if (p.name != "failed") LH_EQ(restored.size(), 2u);
}
