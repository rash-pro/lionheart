// Owned-game exactness: compare the native host against its interpreter fallback.
#include "lh_test.h"
#include <algorithm>
namespace {
struct Receipt {
    std::string state, card, cycles;
    std::map<std::string,std::string> frames;
};
Receipt run(lh::Case &t, const lh::fs::path &native, const lh::fs::path &exe,
            const lh::fs::path &disc, const lh::fs::path &dest, bool interpret) {
    lh::fs::create_directories(dest);
    auto config=dest/"empty.ini";
    lh::write_file(config,"");
    auto cmd=t.target(native).args({exe,"--config",config,"--headless","--fast","--mute",
        "--renderer","soft","--stall","0","--max-instructions","0","--max-vblanks","15000",
        "--input-script",t.root()/"tests/scripts/new-game.txt","--card1",dest/"card.mcr",
        "--savestate-dir",dest/"states","--screenshot-dir",dest/"shots","--dump-state",dest/"state.bin",
        "--dump-frames",dest/"frames","--frame-step","1","--log",lh::fs::path("/dev/null"),"--disc",disc});
    if(interpret) cmd.arg("--interpreter");
    cmd.cwd(dest).env("SDL_VIDEODRIVER","dummy").env("SDL_AUDIODRIVER","dummy")
        .env("XDG_CONFIG_HOME",(dest/"config").string()).env("XDG_DATA_HOME",(dest/"data").string());
    auto result=t.ok(cmd);
    auto text=result.out+result.err;
    lh::write_file(dest/"run.log",text);
    LH_CONTAINS(text,"Stop: VBlank limit reached"); LH_CONTAINS(text,"VBlanks=15000 ");
    Receipt out{lh::sha256_file(dest/"state.bin"),lh::sha256_file(dest/"card.mcr"),
        lh::capture(text,R"(Hardware: cycles=(\d+))"),{}};
    std::vector<lh::fs::path> frames;
    for(auto &entry:lh::fs::directory_iterator(dest/"frames"))
        if(entry.path().extension()==".png") frames.push_back(entry.path());
    std::sort(frames.begin(),frames.end()); LH_EQ(frames.size(),15000u);
    for(auto &path:frames) out.frames[path.filename().string()]=lh::sha256_file(path);
    for(size_t i=0;i+1<frames.size();++i) lh::fs::remove(frames[i]);
    return out;
}
}
LH_GAME_TEST(test_native_matches_interpreter) {
    auto exe=lh::owned_path("SLUS_008.92"), disc=lh::owned_disc(1);
    auto native=t.root()/"build/dev/ff8";
    LH_CHECK_MSG(lh::fs::is_regular_file(native),"run cmake -P tools/setup.cmake -- --disc1 DISC first");
    auto before=run(t,native,exe,disc,t.temp()/"interpreter",true);
    auto after=run(t,native,exe,disc,t.temp()/"native",false);
    LH_EQ(before.state,after.state); LH_EQ(before.card,after.card); LH_EQ(before.cycles,after.cycles);
    LH_EQ(before.frames.size(),after.frames.size());
    for(auto &[name,digest]:before.frames) LH_CHECK_MSG(after.frames.count(name) && after.frames.at(name)==digest,"frame mismatch: "+name);
}
