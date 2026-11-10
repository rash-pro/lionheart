// Real player shell recreation and text input; no guest or visible window.
#include "host_fixtures.h"
namespace {
lh::Result shell(lh::Case &t, const std::string &steps, bool gui=false, bool vulkan=false) {
    auto path=t.temp()/"gui.txt"; lh::write_file(path,steps);
    auto cmd=lh::dummy(t.player()).args({"--config",t.temp()/"lionheart.ini","--gui-test",path}).cwd(t.temp())
      .env("SDL_RENDER_DRIVER","software").env("XDG_DATA_HOME",(t.temp()/"data").string())
      .env("XDG_CONFIG_HOME",(t.temp()/"config").string()).timeout(60);
    if(gui) cmd.args({"--gui","--present-size","1280x720"});
    if(vulkan) cmd.env("VK_DRIVER_FILES","/usr/share/vulkan/icd.d/lvp_icd.json");
    return t.ok(cmd);
}
}
LH_GAME_TEST(test_shell_recreates_both_renderers_and_keeps_text_input) {
    auto disc=lh::owned_disc(1);
    if(!lh::fs::is_regular_file("/usr/share/vulkan/icd.d/lvp_icd.json"))
      lh::skip("owned Disc 1 and lavapipe are required for the player shell gate");
    auto movies=t.temp()/"movies"; lh::fs::create_directory(movies); auto config=t.temp()/"lionheart.ini";
    lh::write_file(config,"disc1="+disc.string()+"\n");
    auto r=shell(t,"wait 20\nsection next\nsection next\nsection next\nsection next\nwait 3\n"
      "assert-focus video.renderer\ndrop \""+movies.string()+"\"\nnav right\nwait 15\nassert-text video.renderer \"Vulkan\"\n"
      "nav right\nwait 15\nassert-text video.renderer \"Soft\"\nsection next\nsection next\nwait 3\nselect\nwait 2\n"
      "nav right\nselect\nwait 3\nassert-focus ra.user\nselect\nwait 2\ntext \"sample-user\"\nwait 2\n"
      "assert-text ra.user \"sample-user\"\nquit\n",true,true);
    LH_CONTAINS(r.err,"GUI test complete:"); LH_CONTAINS(r.err,"Vulkan: llvmpipe");
    LH_NOT_CONTAINS(r.err,"using native soft"); LH_NOT_CONTAINS(r.err,"falling back"); LH_NOT_CONTAINS(r.out,"Hardware:");
    auto text=lh::read_file(config); LH_CONTAINS(text,"renderer=soft"); LH_CONTAINS(text,movies.string()); LH_NOT_CONTAINS(text,"sample-user");
}
LH_GAME_TEST(test_rescan_relocates_missing_disc_without_replacing_verified_media) {
    auto image=lh::owned_disc(1), folder=t.temp()/"discs"; lh::fs::create_directory(folder);
    for(auto e:lh::fs::directory_iterator(image.parent_path())) if(e.is_regular_file())
      lh::fs::create_symlink(lh::fs::canonical(e.path()),folder/e.path().filename());
    auto config=t.temp()/"lionheart.ini"; lh::write_file(config,"disc1="+(folder/"missing.cue").string()+"\n");
    auto r=shell(t,"wait 20\nassert-focus disc.1.locate\nnav left\nassert-focus disc.1.rescan\nselect\nwait 30\n"
      "assert-text disc.1.status \"SLUS-00892 · verified\"\nquit\n");
    LH_CONTAINS(r.err,"GUI test complete:"); auto bin=image; bin.replace_extension(".bin");
    LH_CONTAINS(lh::read_file(config),"disc1="+(folder/bin.filename()).string()); LH_NOT_CONTAINS(r.out,"Hardware:");
}
LH_TEST(test_shell_missing_bin_row_names_the_bin) {
    auto cue=t.temp()/"selected.cue"; lh::write_file(cue,"FILE \"absent.bin\" BINARY\nTRACK 01 MODE2/2352\nINDEX 01 00:00:00\n");
    lh::write_file(t.temp()/"lionheart.ini","disc1="+cue.string()+"\n");
    auto missing=(t.temp()/"absent.bin").generic_string();
    auto r=shell(t,"wait 10\nassert-text disc.1.status \"File not found: "+missing+"\"\nquit\n");
    LH_CONTAINS(r.err,"GUI test complete:"); LH_NOT_CONTAINS(r.out,"Hardware:");
}
