// Desktop launch contracts use public synthetic discs and isolated application data.
#include "synthetic_player.h"
namespace {
using namespace synthetic;
lh::fs::path cue(lh::Case &t, const std::string &name, Disc d = {}) {
    auto image=disc(t.temp()/(name+".bin"),d), path=t.temp()/(name+".cue");
    lh::write_file(path,"FILE \""+image.filename().string()+"\" BINARY\nTRACK 01 MODE2/2352\nINDEX 01 00:00:00\n");
    return path;
}
lh::Command opened(lh::Case &t, const lh::fs::path &path) {
    // A standalone %f expands to one argv element, without shell interpretation.
    auto entry=lh::read_file(t.root()/"packaging/linux/lionheart.desktop");
    LH_CONTAINS(entry,"\nExec=lionheart %f\n");
    LH_CONTAINS(entry,"\nMimeType=application/x-cue;\n");
    return t.target(player(t)).arg(path).cwd(t.temp()).timeout(30)
        .env("HOME",t.temp()).env("XDG_DATA_HOME",t.temp()/"data")
        .env("XDG_CONFIG_HOME",t.temp()/"config")
        .env("SDL_VIDEODRIVER","dummy").env("SDL_AUDIODRIVER","dummy");
}
}
LH_TEST(test_desktop_entry_validates) {
    auto available=t.run(t.host("sh").args({"-c","command -v desktop-file-validate"}));
    if(available.rc) lh::skip("desktop-file-validate is not installed (desktop-file-utils)");
    t.ok(t.host("desktop-file-validate").arg(t.root()/"packaging/linux/lionheart.desktop"));
}
LH_TEST(test_desktop_single_local_path_reaches_game) {
    auto path=cue(t,"Disco é #1 ; $ prueba");
    auto cmd=opened(t,path); run(cmd); auto positional=t.ok(cmd);
    auto explicit_disc=t.ok(running(t,{"--disc",path}));
    LH_CONTAINS(positional.out,"Stop: VBlank limit reached");
    LH_CONTAINS(explicit_disc.out,"Stop: VBlank limit reached");
}
LH_TEST(test_desktop_launcher_expands_one_argument) {
    auto available=t.run(t.host("sh").args({"-c","command -v gio"}));
    if(available.rc) lh::skip("gio is not installed; local desktop expansion requires GLib utilities");
    auto binary=player(t), path=cue(t,"Disco é #1 ; $ prueba");
    auto bin=t.temp()/"bin"; lh::fs::create_directory(bin);
    auto wrapper=bin/"lionheart", args=t.temp()/"argv", log=t.temp()/"launch.log", done=t.temp()/"done";
    lh::write_file(wrapper,"#!/bin/sh\nprintf '%s\\n' \"$#\" \"$1\" > \"$LH_DESKTOP_ARGV\"\n"
        "\"$LH_DESKTOP_PLAYER\" \"$@\" --config /dev/null --headless --fast --mute --max-vblanks 2 > \"$LH_DESKTOP_LOG\" 2>&1\n"
        "printf '%s' \"$?\" > \"$LH_DESKTOP_DONE\"\n");
    lh::fs::permissions(wrapper,lh::fs::perms::owner_all);
    t.ok(t.host("gio").args({"launch",t.root()/"packaging/linux/lionheart.desktop",path})
        .env("PATH",bin.string()+":/usr/bin:/bin").env("LH_DESKTOP_PLAYER",binary)
        .env("LH_DESKTOP_ARGV",args).env("LH_DESKTOP_LOG",log).env("LH_DESKTOP_DONE",done)
        .env("HOME",t.temp()).env("XDG_DATA_HOME",t.temp()/"data").env("XDG_CONFIG_HOME",t.temp()/"config")
        .env("SDL_VIDEODRIVER","dummy").env("SDL_AUDIODRIVER","dummy")
        .unset("DBUS_SESSION_BUS_ADDRESS").timeout(30));
    t.ok(t.host("sh").args({"-c","i=0; while [ ! -f \"$1\" ] && [ $i -lt 300 ]; do sleep .1; i=$((i+1)); done; test -f \"$1\"","wait",done}).timeout(35));
    LH_TEXT_EQ(lh::read_file(args),"1\n"+path.string()+"\n");
    LH_TEXT_EQ(lh::read_file(done),"0"); LH_CONTAINS(lh::read_file(log),"Stop: VBlank limit reached");
}
LH_TEST(test_desktop_keeps_other_known_discs) {
    auto one=cue(t,"one"); Disc d; d.number=2; auto two=cue(t,"two",d);
    auto config=t.temp()/"settings.ini";
    lh::write_file(config,"disc1="+one.string()+"\n");
    auto cmd=opened(t,two); cmd.args({"--config",config}); run(cmd);
    auto result=t.ok(cmd); LH_CONTAINS(result.out,"Stop: VBlank limit reached");
    LH_CONTAINS(lh::read_file(config),"disc1="+one.string());
}
namespace {
struct DesktopError { const char *id,*message; bool configured=false; };
const DesktopError desktop_errors[]={
    {"other-game","Another game: SCUS_123.45"},
    {"damaged","FF8 USA EXE size check failed (SLUS_008.92)"},
    {"unsupported","supported:"},
    {"other-game-configured","Another game: SCUS_123.45",true},
    {"damaged-configured","FF8 USA EXE size check failed (SLUS_008.92)",true},
    {"unsupported-configured","supported:",true}};
}
LH_TEST_P(test_desktop_rejected_image_shows_error,desktop_errors) {
    Disc d; if(std::string(p.id).find("other-game")==0) { d.name="SCUS_123.45"; d.image=false; }
    if(std::string(p.id).find("damaged")==0) d.exe_size=2048;
    auto path=cue(t,"selected",d);
    if(std::string(p.id).find("unsupported")==0) lh::write_file(path,"FILE \"selected.bin\" BINARY\nTRACK 01 MODE1/2048\nINDEX 01 00:00:00\n");
    auto headless=opened(t,path); run(headless); auto error=t.run(headless);
    LH_EQ(error.rc,2); LH_CONTAINS(error.err,p.message);
    auto script=t.temp()/"gui.txt";
    std::string detail=error.err; while(!detail.empty() && detail.back()=='\n') detail.pop_back();
    lh::write_file(script,"wait 10\nassert-text startup.error \""+detail+"\"\nquit\n");
    auto cmd=opened(t,path); cmd.args({"--gui-test",script});
    if(p.configured) {
        auto one=cue(t,"known"), config=t.temp()/"known.ini";
        lh::write_file(config,"disc1="+one.string()+"\n"); cmd.args({"--config",config});
        lh::write_file(script,"wait-text disc.1.status \"SLUS-00892 · verified\" 600\nassert-text startup.error \""+detail+"\"\nquit\n");
    }
    auto result=t.ok(cmd); LH_CONTAINS(result.err,"GUI test complete:");
    LH_NOT_CONTAINS(result.out,"Hardware:");
}
