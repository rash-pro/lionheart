// Synthetic overlapping registries retain the linear dispatch semantics.
#include "lh_test.h"

namespace {
struct Row { std::string id, mode; uint32_t seed; };
std::vector<Row> rows() {
    std::vector<Row> out;
    for (const char *mode : {"entry", "precise"})
        for (uint32_t seed : {1u, 0x9E3779B9u, 0xDEADBEEFu})
            out.push_back({std::string(mode) + "-" + std::to_string(seed), mode, seed});
    return out;
}
LH_TEST_P(test_dispatch_index_matches_linear_catalog, rows()) {
    t.ok(t.harness(p.mode).args({10000, p.seed}).timeout(60));
}
}
