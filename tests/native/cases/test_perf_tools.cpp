// Receipts bind a scene to independent save-card inputs and exact boundaries.
#include "lh_test.h"
#include "../../../tools/devtools/perf_report.h"
LH_TEST(test_scene_counter_deltas_require_exact_boundaries) {
    std::string log="[ 0.1] CPU Extensions Detected:\n[ 0.123] CPU MEMORY vb=30 ram=10 scratch=1\nCPU DISPATCH vb=30 call=4 candidate_lost=0\nCPU MEMORY vb=60 ram=40 scratch=21\nCPU DISPATCH vb=60 call=14 candidate_lost=0";
    auto result=perf::summarize(log,30,60);
    LH_TEXT_EQ(result.at("counters").dump(),"{\"memory.ram\": 30, \"memory.scratch\": 20, \"dispatch.call\": 10, \"dispatch.candidate_lost\": 0}");
    bool caught=false;
    try { perf::summarize(log,31,60); } catch(const std::runtime_error &e) { caught=true; LH_CONTAINS(e.what(),"missing exact"); }
    LH_CHECK(caught);
}
