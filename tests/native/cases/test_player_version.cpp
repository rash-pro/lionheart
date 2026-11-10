// The real entry point must answer before config, data paths or SDL startup.
#include "host_fixtures.h"
#include <regex>
namespace {
lh::Command player(lh::Case &t) {
    return t.player().cwd(t.temp()).timeout(20)
        .env("HOME",t.temp()).env("APPDATA",t.temp()/"data").env("XDG_DATA_HOME",t.temp()/"data")
        .env("XDG_CONFIG_HOME",t.temp()/"config")
        .env("SDL_VIDEODRIVER","unavailable-version-test-driver")
        .env("SDL_AUDIODRIVER","unavailable-version-test-driver");
}
}
LH_TEST(version_precedes_all_startup) {
    auto source=lh::read_file(t.root()/"CMakeLists.txt"); std::smatch match;
    LH_CHECK(std::regex_search(source,match,std::regex("project\\(lionheart VERSION ([0-9.]+)")));
    auto prefix="Lionheart "+match[1].str();
    auto initial=t.ok(player(t).arg("--version"));
    LH_EQ(initial.err,"");
    LH_CHECK(initial.out==prefix+"\n" || std::regex_match(initial.out,
        std::regex("Lionheart "+match[1].str()+" \\([0-9a-f]+(-dirty)?\\)\n")));
    LH_CHECK(lh::fs::is_empty(t.temp()));
    auto config=t.temp()/"broken.ini";
    lh::write_file(config,"renderer=invalid\nscale=garbage\ndisc1=absent\nra=invalid\n");
    lh::fs::create_directory(t.temp()/"data");
    for(auto path:{config,t.temp()/"missing.ini"}) {
        auto r=t.ok(player(t).args({"--toolchain-install","--config",path,"--unknown",
            "--version","--disc","absent","--gui"}));
        LH_TEXT_EQ(r.out,initial.out); LH_EQ(r.err,"");
        LH_CHECK(lh::fs::is_empty(t.temp()/"data"));
        LH_CHECK(!lh::fs::exists(t.temp()/"config"));
    }
    // Invalid video drivers fail SDL initialization, so a successful diagnostic
    // above cannot have opened a window. A normal session still logs its identity.
    auto r=t.run(lh::dummy(player(t)).args({"--headless","--config",t.temp()/"missing.ini"}));
    LH_EQ(r.rc,2);
    auto folder=lh::fs::path(t.player().program()).extension()==".exe" ? "Lionheart" : "lionheart";
    auto log=lh::read_file(t.temp()/"data"/folder/"logs/lionheart.log");
    LH_TEXT_EQ(log.substr(0,initial.out.size()),initial.out);
    LH_CHECK(log.find("Lionheart ",initial.out.size())==std::string::npos);
}
