// The optional Linux probe owns its sampler through CPU replacement and teardown.
#include "host_fixtures.h"
#include <sys/utsname.h>
static const char *const modes[] = {"False", "True"};
LH_TEST_P(test_frame_probe_survives_cpu_replacement, modes) {
    utsname host{};
    uname(&host);
    if (std::string(host.sysname) != "Linux" || std::string(host.machine) != "x86_64")
        lh::skip("Linux x86-64 diagnostic probe");
    auto csv = t.temp() / "frames.csv";
    auto driver=t.harness("driver");
    auto frozen=lh::fs::path(driver.program()).parent_path().parent_path().parent_path()/"frame-probe-frozen";
    auto probe=t.temp()/"probe";
    driver.args({frozen,"--output",probe});
    if(std::string(p)=="True") driver.arg("--subsystems");
    LH_EQ(t.ok(driver).out,probe.string()+"\n");
    t.ok(t.target(probe).env("FF8_FRAME_TIMINGS", csv.string()));
    auto rows = lh::lines(lh::read_file(csv));
    LH_EQ(rows.size(), 4u);
    auto split = [](std::string row) {
        std::replace(row.begin(), row.end(), ',', '\n');
        return lh::lines(row);
    };
    auto header = split(rows[0]);
    for (int n = 1; n <= 3; ++n) {
        auto values = split(rows[n]);
        auto field = [&](const std::string &key) {
            auto pos = std::find(header.begin(), header.end(), key);
            LH_CHECK(pos != header.end());
            return std::stoll(values.at(pos - header.begin()));
        };
        LH_EQ(field("vblank"), n);
        LH_EQ(field("cycles"), 10);
        LH_EQ(field("interpreted"), 5);
    }
    if (std::string(p) == "True") LH_EQ(lh::read_file(csv.string() + ".samples").rfind("# cpu_init=", 0), 0u);
}
