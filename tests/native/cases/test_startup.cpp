// Host review regressions: player-only policy and lossless path punctuation.
#include "synthetic_player.h"
namespace {
using namespace synthetic;
}
LH_TEST(test_dev_keeps_unidentified_arbitrary_extension_and_order) {
    auto binary=player(t,true), one=t.temp()/"first.raw", two=t.temp()/"second.data";
    lh::write_file(one,std::string(2352*32,'\0')); lh::write_file(two,"invalid second image, never opened at startup");
    auto exe=binary.parent_path().parent_path()/"guest.exe";
    for (bool both : {false,true}) {
        auto cmd=t.target(binary).args({exe,"--config","/dev/null"}).env("SDL_VIDEODRIVER","dummy").env("SDL_AUDIODRIVER","dummy");
        run(cmd); cmd.args({"--disc",one}); if(both) cmd.args({"--disc",two});
        auto r=t.ok(cmd); LH_NOT_CONTAINS(r.err,"not a PlayStation");
    }
    auto cmd=t.target(binary).args({exe,"--config","/dev/null"}).env("SDL_VIDEODRIVER","dummy").env("SDL_AUDIODRIVER","dummy");
    run(cmd); cmd.args({"--disc",two,"--disc",one}); auto r=t.run(cmd); LH_EQ(r.rc,2); LH_CONTAINS(r.err,two.string());
}
static const char *const extensions[]={".bin",".BIN",".cue",".CUE"};
LH_TEST_P(test_positional_disc,extensions) {
    auto image=disc(t.temp()/"one.bin"); std::string ext=p;
    if(ext==".cue"||ext==".CUE") { image=t.temp()/("one"+ext); lh::write_file(image,"FILE \"one.bin\" BINARY\nTRACK 01 MODE2/2352\nINDEX 01 00:00:00\n"); }
    else if(ext==".BIN") { auto dest=t.temp()/"one.BIN"; lh::fs::rename(image,dest); image=dest; }
    t.ok(running(t,{image}));
}
LH_TEST(test_explicit_paths_without_home) {
    auto image=disc(t.temp()/"one.bin");
    t.ok(running(t,{"--disc",image,"--card1",t.temp()/"card.mcr","--savestate-dir",t.temp()/"states"}).env("HOME","").env("XDG_DATA_HOME","").env("XDG_CONFIG_HOME",""));
}
LH_TEST(test_player_path_punctuation) {
    disc(t.temp()/"FF8 #1.cue.bin"); auto cue=t.temp()/"FF8 #1.cue", config=t.temp()/"settings.ini";
    lh::write_file(cue,"FILE \"FF8 #1.cue.bin\" BINARY\nTRACK 01 MODE2/2352\nINDEX 01 00:00:00\n");
    lh::write_file(config,"  # comment\n disc1=FF8 #1.cue\ncard1=card;one.mcr\nfilter=nearest ; comment\n");
    t.ok(running(t,{"--config",config})); LH_CHECK(lh::fs::exists(t.temp()/"card;one.mcr"));
}
namespace { struct Option { const char *id,*option,*value; }; const Option options[]={
    {"FF8_VULKAN-OFF","FF8_VULKAN","OFF"},{"FF8_PGXP-OFF","FF8_PGXP","OFF"},{"FF8_STATS-ON","FF8_STATS","ON"}}; }
LH_TEST_P(test_player_rejects_contradictory_cache,options) {
    lh::write_file(t.temp()/"CMakeCache.txt",std::string(p.option)+":BOOL="+p.value+"\n");
    auto r=t.run(t.host("cmake").args({"-S",t.root(),"-B",t.temp(),"-DFF8_PLAYER=ON"}));
    LH_CHECK(r.rc!=0); LH_CONTAINS(r.err,"FF8_PLAYER requires");
}
LH_TEST(test_config_path_limit) { auto r=t.run(play(t,{"--config",std::string(4096,'x')})); LH_EQ(r.rc,2); LH_CONTAINS(r.err,"4095 bytes"); }
LH_TEST(test_bom_cue_reaches_player_runtime) {
    disc(t.temp()/"one.bin"); auto cue=t.temp()/"one.cue";
    std::string content="\xef\xbb\xbf" "FILE \"one.bin\" BINARY\nTRACK 01 MODE2/2352\nINDEX 01 00:00:00\n";
    lh::write_file(cue,content); t.ok(running(t,{"--disc",cue})); LH_BYTES_EQ(lh::read_file(cue),content);
}
LH_TEST(test_headless_gui_flag_retains_no_disc_error) { auto r=t.run(running(t,{"--gui"})); LH_EQ(r.rc,2); LH_CONTAINS(r.err,"Provide FF8 USA disc 1"); }
LH_TEST(test_long_cli_disc_path_is_not_limited_by_ini_value_storage) {
    auto folder=t.temp(); while((folder/"one.bin").string().size()<540) folder/="long-"+std::string(70,'x');
    lh::fs::create_directories(folder); auto image=disc(folder/"one.bin"); t.ok(running(t,{"--disc",image}));
}
LH_TEST(test_long_validated_player_setting_round_trips) {
    auto image=disc(t.temp()/"one.bin"), config=t.temp()/"long-setting.ini";
    lh::write_file(config,"screenshot-dir="+std::string(500,'a')+"\n"); t.ok(running(t,{"--config",config,"--disc",image}));
}
LH_TEST(test_player_path_markers_are_data_for_every_path_option) {
    auto image=disc(t.temp()/"one.bin"), config=t.temp()/"markers.ini", dest=t.temp()/"screens #1;two";
    lh::write_file(config,"screenshot-dir="+dest.string()+" # comment\n"); t.ok(running(t,{"--config",config,"--disc",image,"--screenshot-at","0"}));
    int count=0; for(auto &e:lh::fs::directory_iterator(dest)) if(e.path().filename().string().rfind("lionheart-",0)==0&&e.path().extension()==".png") ++count;
    LH_EQ(count,1);
}
