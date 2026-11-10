// Timing reports reject incomplete work and count threshold crossings exactly.
#include "lh_test.h"
#include "../../../tools/devtools/frame_bench.h"
LH_TEST(test_frame_distribution) {
    LH_TEXT_EQ(bench::distribution({1,16.7,16.8,33,33.1}).dump(),"{\"p50\": 16.8, \"p95\": 33.1, \"p99\": 33.1, \"max\": 33.1, \"above_16_7\": 3, \"above_33\": 1}");
    bool caught=false;try{bench::distribution({});}catch(const std::runtime_error &e){caught=true;LH_CONTAINS(e.what(),"empty");}LH_CHECK(caught);
}
static bench::Args args(lh::Case &t) {
    bench::Args a;a.card=t.temp()/"card";a.script=t.temp()/"script";a.exe=t.harness("host").program();a.first=1;a.last=2;lh::write_file(a.card,std::string(128,0));lh::write_file(a.script,"1 -\n");return a;
}
static const char *complete[]={"True","False"};
LH_TEST_P(test_frame_receipt_requires_every_vblank,complete) {
    auto a=args(t);bool good=std::string(p)=="True";if(!good)setenv("INCOMPLETE","1",1);
    bool caught=false;try {auto r=bench::measure(a.exe,a,t.temp()/"out");LH_CHECK(std::get<bool>(r.at("complete").value));LH_CHECK(std::get<bool>(r.at("source_card_unchanged").value));LH_EQ(std::get<int64_t>(r.at("frames").value),2);LH_EQ(std::get<int64_t>(r.at("cycles").value),20);LH_CHECK(!std::get<std::string>(r.at("load_before").value).empty());LH_CHECK(!std::get<std::string>(r.at("load_after").value).empty());}catch(const std::runtime_error &e){if(good)throw;caught=true;LH_CONTAINS(e.what(),"missing or repeated");}LH_EQ(caught,!good);
}
LH_TEST(test_checkpoint_still_requires_warmup_and_contiguous_frames) {
    auto a=args(t);setenv("CHECKPOINT","1",1);a.first=6;a.last=6;auto state=t.temp()/"state";lh::write_file(state,"synthetic checkpoint");auto r=bench::measure(a.exe,a,t.temp()/"good",state);LH_EQ(std::get<int64_t>(r.at("frames").value),1);
    a.first=5;bool caught=false;try{bench::measure(a.exe,a,t.temp()/"bad",state);}catch(const std::runtime_error &e){caught=true;LH_CONTAINS(e.what(),"missing or repeated");}LH_CHECK(caught);
}
