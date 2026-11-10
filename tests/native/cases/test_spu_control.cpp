// Exact observable state across bulk/control DSP and dependency changes.
#include "lh_test.h"

static const char *const kModes[] = {"exact", "play"};

LH_TEST_P(test_control_matches_full_model, kModes) {
    t.ok(t.harness(p).timeout(90));
}

struct Trace { std::string id; const char *mode; int bytes; };
static std::vector<Trace> traces() {
    std::vector<Trace> rows;
    for (const auto mode : kModes)
        for (int n = 0; n < 12; ++n)
            rows.push_back({std::string(mode) + "-" + std::to_string(n), mode, n});
    return rows;
}

LH_TEST_P(test_replay_rejects_incomplete_or_empty_trace, traces()) {
    const auto trace = t.temp() / "incomplete.bin";
    lh::write_file(trace, "FF8SPUT1" + std::string(p.bytes, '\1'));
    const auto result = t.run(t.harness(p.mode).arg(trace));
    LH_EQ(result.rc, 2);
    LH_NOT_CONTAINS(result.out, "PASS");
}
