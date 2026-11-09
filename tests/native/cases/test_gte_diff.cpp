// The trace replayer rejects missing work and compares every raw register.
#include "lh_test.h"
static const char *modes[]={"exact","play"};
LH_TEST_P(test_extreme_command_sequences,modes) {
    auto r=t.ok(t.harness(p));LH_CONTAINS(r.out,"GTE SYNTHETIC PASS commands=100000");LH_CONTAINS(r.out,"GTE NCLIP PASS corners=729");LH_CONTAINS(r.out,"GTE DIFF PASS");
}
struct Damage {std::string id,mode,damage;};
static std::vector<Damage> damages() {std::vector<Damage> out;for(const char *m:modes)for(const char *d:{"None","header","empty","truncated","output","flag"})out.push_back({std::string(m)+"-"+d,m,d});return out;}
LH_TEST_P(test_recorded_outputs_and_trace_integrity,damages()) {
    std::string data("FF8GTE1\0",8);data.resize(8+131*4,0);
    auto word=[&](int index,uint32_t value){for(int i=0;i<4;++i)data[8+index*4+i]=char(value>>(i*8));};
    word(0,2400);word(1,0x80010000);word(2,0x4a000006);
    if(p.damage=="output")word(67+24,1);if(p.damage=="flag")word(99+31,0x80000000);
    if(p.damage=="header")data.replace(0,8,"invalid!");else if(p.damage=="empty")data.resize(8);else if(p.damage=="truncated")data.pop_back();
    auto trace=t.temp()/"trace.gte";lh::write_file(trace,data);auto r=t.run(t.harness(p.mode).arg(trace));LH_EQ(r.rc,p.damage=="None"?0:2);LH_CONTAINS(r.out,p.damage=="None"?"GTE TRACE PASS":"GTE TRACE FAIL");
}
struct Observer {std::string id,mode,specialized;};
static std::vector<Observer> observers(){std::vector<Observer> out;for(const char *m:modes)for(const char *s:{"False","True"})out.push_back({std::string(m)+"-"+s,m,s});return out;}
LH_TEST_P(test_trace_observer_bounds_and_complete_output,observers()) {
    auto trace=t.temp()/"observed.gte";auto cmd=t.harness("record_"+p.specialized).env("FF8_GTE_TRACE",trace.string()).env("FF8_GTE_TRACE_FIRST","1").env("FF8_GTE_TRACE_LAST","2");
    auto r=t.ok(cmd);LH_CONTAINS(r.out,"records=2");r=t.ok(t.harness(p.mode).arg(trace));LH_CONTAINS(r.out,"records=2");cmd.env("FF8_GTE_TRACE_LIMIT","1");r=t.run(cmd);LH_EQ(r.rc,2);LH_CONTAINS(r.err,"command limit reached");
}
