// Observe the complete CPU object inside the actual player's modal pause loop.
#include "player_probe.h"
#include "host_fixtures.h"
static const char *renderers[]={"soft","vulkan"};
LH_GAME_TEST_P(test_actual_modal_loop_freezes_every_cpu_byte,renderers) {
    const auto real=lh::fs::canonical(t.player().program());const auto probe=lh::pause_player(t,real,t.temp()/"probe-build");
    const auto disc=lh::owned_disc(1);const lh::fs::path icd="/usr/share/vulkan/icd.d/lvp_icd.json";
    const auto source=lh::owned_path("saves/disc1-start-maxed.mcr");
    if(!lh::fs::is_regular_file(source) || (std::string(p)=="vulkan" && !lh::fs::is_regular_file(icd)))
        lh::skip("owned Disc 1/card and the selected renderer are required");
    const auto original=lh::sha256_file(source);const auto config=t.temp()/"empty.ini",script=t.temp()/"gui.txt";
    lh::write_file(config,"");std::string instructions;for(int frame:{300,900,1500}) instructions+="at "+std::to_string(frame)+"\nopen\nwait 4\nclose\n";lh::write_file(script,instructions);
    std::vector<std::string> states;
    for(auto name:{"control","probe","fault"}) {
        const bool control=std::string(name)=="control",fault=std::string(name)=="fault";auto binary=control?real:probe;
        auto out=t.temp()/name;lh::fs::create_directory(out);auto card=out/"card.mcr";lh::fs::copy_file(source,card);
        auto command=t.target(binary).args({"--config",config,"--headless","--fast","--mute","--stall","0","--max-instructions","0","--max-vblanks","2000",
            "--log","/dev/null","--disc",disc,"--card1",card,"--savestate-dir",out/"states","--renderer",p,"--present-size","1280x720","--dump-state",out/"state.bin",
            "--input-script",t.root()/"tests/scripts/profile-field.txt"});
        if(!control) command.args({"--gui-test",script});
        command=lh::dummy(command).cwd(out).env("VK_DRIVER_FILES",icd.string()).env("XDG_DATA_HOME",(out/"data").string()).timeout(180);
        if(fault) command.env("FF8_GUI_PAUSE_PROBE_STEP","1");auto result=t.run(command);lh::write_file(out/"run.log",result.out+result.err);
        dt::Json args=dt::Json::array();args.push(binary.string());for(const auto &arg:command.arguments()) args.push(arg.text());
        lh::write_file(out/"receipt.json",dt::Json(dt::Json::Object{{"command",args},{"exit",result.rc},{"binary_sha256",lh::sha256_file(binary)},{"injected_retirement",fault}}).dump(2)+"\n");
        if(fault) {LH_EQ(result.rc,99);LH_CONTAINS(result.err,"guest changed inside modal loop");continue;}
        LH_CHECK_MSG(result.rc==0,result.out+result.err);LH_CONTAINS(result.out,"Stop: VBlank limit reached");
        if(!control) {
            const std::string pattern="GUI pause proof: opens=([0-9]+) frames=([0-9]+) frozen=1";
            LH_EQ(std::stoi(lh::capture(result.err,pattern,1)),3);LH_CHECK(std::stoi(lh::capture(result.err,pattern,2))>=12);LH_CONTAINS(result.err,"GUI test complete:");
        }
        states.push_back(lh::read_file(out/"state.bin"));
    }
    LH_BYTES_EQ(states.at(0),states.at(1));LH_EQ(lh::sha256_file(source),original);
}
