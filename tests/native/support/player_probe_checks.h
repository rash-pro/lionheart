// Exercise release-command replay and both observer outcomes without game data.
#pragma once
#include "player_probe.h"
#include "equiv.h"
LH_TEST(pause_probe_replays_the_player_compile_and_link_commands) {
    for(bool arguments:{false,true}) {
        const char *equiv=std::getenv("LH_EQUIV_DIR");
        auto build=equiv?lh::fs::path(equiv)/(arguments?"pause-arguments":"pause-command"):t.temp()/(arguments?"arguments host":"command host");lh::fs::create_directories(build/"CMakeFiles/ff8.dir");
        auto include=t.root()/"runtime",host=t.root()/"backends/host";
        lh::write_file(build/"cpu.c","#include \"cpu.h\"\n#include <string.h>\nvoid cpu_init(cpu_t *c){memset(c,0,sizeof(*c));}\nvoid host_gui_frame(unsigned w,unsigned h){(void)w;(void)h;}\n");
        lh::write_file(build/"gui.c","void host_gui_frame(unsigned,unsigned);\nvoid host_gui_open(void){for(int i=0;i<4;++i)host_gui_frame(640,480);}\n");
        lh::write_file(build/"main.c","#include \"cpu.h\"\nvoid host_gui_open(void);\nint main(void){static cpu_t cpu;cpu_init(&cpu);for(int i=0;i<3;++i)host_gui_open();return 0;}\n");
        auto quote=[](const std::string &s) {std::string out="'";for(char c:s) out+=c=='\''?"'\\''":std::string(1,c);return out+"'";};
        auto shell=[&](const std::vector<std::string> &cmd) {std::string text;for(const auto &s:cmd) {if(!text.empty()) text+=' ';text+=quote(s);}return text;};
        std::vector<std::string> base={"cc","-std=c99","-O2","-DFF8_PGXP","-DFF8_PLAYER","-DFF8_GUI","-I"+include.string(),"-I"+host.string()};
        auto sdl=trace::split(t.ok(t.host("pkg-config").args({"--cflags","sdl2"})).out);base.insert(base.end(),sdl.begin(),sdl.end());
        std::vector<std::string> compile;
        for(auto name:{"cpu","gui","main"}) {
            compile=base;compile.insert(compile.end(),{"-c",(build/(std::string(name)+".c")).string(),"-o",(build/(std::string(name)+".o")).string()});lh::run_arguments(t,compile,build);
        }
        compile.insert(compile.end(),{"-MD","-MMD","-MP","-MF",(build/"must-stay.d").string(),"-MT","main.o","-MQ","main.o"});
        auto recorded=compile;lh::replace_operand(recorded,"-c",t.root()/"backends/host/main.c");
        lh::Json::Object row={{"directory",build.string()},{"file",(t.root()/"backends/host/main.c").string()}};
        if(arguments) {lh::Json::Array array;for(const auto &s:recorded) array.emplace_back(s);row.emplace("arguments",array);}else row.emplace("command",shell(recorded));
        lh::write_file(build/"compile_commands.json",lh::Json(lh::Json::Array{lh::Json(row)}).dump());
        std::vector<std::string> link={"cc",(build/"main.o").string(),(build/"cpu.o").string(),(build/"gui.o").string(),"-o",(build/"lionheart").string(),"-Wl,--dependency-file="+(build/"link.d").string()};
        lh::write_file(build/"CMakeFiles/ff8.dir/link.txt",shell(link));lh::run_arguments(t,link,build);
        lh::write_file(build/"must-stay.d","unchanged\n");
        auto binary=lh::pause_player(t,build/"lionheart",build/"probe");
        auto run=t.ok(t.host(binary.string()));LH_CONTAINS(run.err,"GUI pause proof: opens=3 frames=12 frozen=1");
        run=t.run(t.host(binary.string()).env("FF8_GUI_PAUSE_PROBE_STEP","1"));LH_EQ(run.rc,99);LH_CONTAINS(run.err,"guest changed inside modal loop");
        LH_TEXT_EQ(lh::read_file(build/"must-stay.d"),"unchanged\n");
        if(equiv) {
            lh::equiv::record(t,uint64_t(binary.string().size()),binary.string());
        }
    }
}
