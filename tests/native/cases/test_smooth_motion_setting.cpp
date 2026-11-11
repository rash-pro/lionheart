// Smooth motion's setting (docs/frame-present.md): read from the config and
// the command line like the other Video settings, and switched and stored from
// the Video menu.
#include "host_fixtures.h"
namespace {
lh::Command options(lh::Case &t, std::initializer_list<lh::Arg> args) {
    // The parser resolves adjacent settings from its executable, not its cwd.
    auto binary=t.temp()/("options" + lh::fs::path(t.harness("options").program()).extension().string());
    lh::fs::copy_file(t.harness("options").program(),binary,lh::fs::copy_options::overwrite_existing);
    auto cmd=t.target(binary).arg("unused.exe").args(args).env("HOME",t.temp()).env("XDG_CONFIG_HOME",t.temp()/"xdg")
      .env("FF8_TEST_SMOOTH","1");
    bool config=false; for (const auto &a:args) if(a.text()=="--config") config=true;
    if(!config) { auto empty=t.temp()/"empty.ini"; lh::write_file(empty,""); cmd.args({"--config",empty}); }
    return cmd;
}
std::string last(const lh::Result &r) { return lh::lines(r.out).back(); }
}
LH_TEST(test_smooth_motion_from_config_and_cli) {
    auto run=[&](std::initializer_list<lh::Arg> args){ return t.run(options(t,args)); };
    LH_EQ(last(run({})),"smooth=0");
    auto path=t.temp()/"smooth.ini"; lh::write_file(path,"smooth-motion=1\n");
    LH_EQ(last(run({"--config",path})),"smooth=1");
    LH_EQ(last(run({"--config",path,"--smooth-motion","0"})),"smooth=0");
    LH_EQ(last(run({"--smooth-motion"})),"smooth=1");
    LH_EQ(last(run({"--smooth-motion","1","--fullscreen"})),"smooth=1");
    // Classed with the other presentation settings.
    LH_CONTAINS(run({}).out,"\nsmooth-motion P\n");
    lh::write_file(path,"smooth-motion=yes\n"); LH_EQ(run({"--config",path}).rc,2);
    LH_EQ(run({"--smooth-motion","2"}).rc,2);
}
LH_TEST(test_video_menu_switches_smooth_motion) {
    auto cue=t.temp()/"selected.cue"; lh::write_file(cue,"FILE \"absent.bin\" BINARY\nTRACK 01 MODE2/2352\nINDEX 01 00:00:00\n");
    lh::write_file(t.temp()/"lionheart.ini","disc1="+cue.string()+"\n");
    std::string down; for(int i=0;i<5;++i) down+="nav down\nwait 2\n";
    auto path=t.temp()/"gui.txt";
    lh::write_file(path,"wait 10\nsection next\nsection next\nwait 3\nassert-focus video.renderer\nassert-text video.smooth \"Off\"\n"+down+
      "assert-focus video.smooth\nselect\nwait 3\nassert-text video.smooth \"On\"\nquit\n");
    auto r=t.ok(lh::dummy(t.player()).args({"--config",t.temp()/"lionheart.ini","--gui-test",path}).cwd(t.temp())
      .env("SDL_RENDER_DRIVER","software").env("XDG_DATA_HOME",(t.temp()/"data").string())
      .env("XDG_CONFIG_HOME",(t.temp()/"config").string()).timeout(60));
    LH_CONTAINS(r.err,"GUI test complete:"); LH_NOT_CONTAINS(r.out,"Hardware:");
    LH_CONTAINS(lh::read_file(t.temp()/"lionheart.ini"),"smooth-motion=1");
}
