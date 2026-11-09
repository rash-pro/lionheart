// Diagnostic paths and exact per-PC counts remain separate from guest state.
#include "generated.h"
#include <array>
#include <cstdlib>
#include <cstdio>
using namespace lh::gen;
namespace {
const char *const stats[] = {"False", "True"};
struct Env {
    std::string name, old;
    bool existed;
    Env(const char *key, const std::string &value) : name(key), existed(std::getenv(key) != nullptr) {
        if (existed) old = std::getenv(key);
        setenv(key, value.c_str(), 1);
    }
    ~Env() { if (existed) setenv(name.c_str(), old.c_str(), 1); else unsetenv(name.c_str()); }
};
}
LH_TEST_P(test_memory_and_transfer_counters, stats) {
    auto check = t.library(p).fn<void(uint64_t *)>("check");
    std::array<uint64_t,11> counts{}, expected{};
    if (std::string(p) == "True") expected = {3,1,1,1,1,1,1,1,1,1,2};
    check(counts.data());
    for (size_t i=0;i<counts.size();++i) LH_EQ(counts[i], expected[i]);
}
LH_TEST(test_compact_pc_histogram_matches_interpreter) {
    Words words = {imm(9,8,8,-1), imm(5,8,0,-2), 0, r(8,31), 0};
    auto data = pack(words);
    auto lib = Build(t).program(Program{Image(data,0x80010000,0x80010000)})
        .probe("test_perf_stats_histogram.c").flags({"-std=c99","-O2","-DFF8_STATS","-DFF8_COMPACT"})
        .runtime({"cpu.c","interp.c"}).library();
    auto check = lib.fn<void(const void *,unsigned)>("check");
    Env first("FF8_PC_FIRST","10"), last("FF8_PC_LAST","11");
    for (unsigned interpret : {0u,1u}) {
        auto path=t.temp()/(std::to_string(interpret)+".csv");
        Env profile("FF8_PC_PROFILE",path.string());
        check(data.data(),interpret);
        std::istringstream csv(lh::read_file(path));
        std::string line;
        auto next = [&] { LH_CHECK(bool(std::getline(csv,line))); if (!line.empty() && line.back()=='\r') line.pop_back(); return line; };
        LH_TEXT_EQ(next(), "pc,instructions");
        const int counts[] = {3,3,3,1,1};
        for (unsigned i=0;i<5;++i) {
            char row[64]; std::snprintf(row,sizeof row,"%08x,%d",0x80010000+i*4,counts[i]);
            LH_TEXT_EQ(next(),row);
        }
        LH_CHECK(!std::getline(csv,line));
    }
}
