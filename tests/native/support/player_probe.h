// Recompile the pause observer with the real main unit's ABI and feature flags.
#pragma once
#include "lh_test.h"
#include "json_parse.h"
#include "../../../tools/devtools/frame_probe.h"
namespace lh {
inline void replace_operand(std::vector<std::string> &command,const std::string &flag,const fs::path &value) {
    auto i=std::find(command.begin(),command.end(),flag);LH_CHECK_MSG(i!=command.end() && i+1!=command.end(),"build command lacks "+flag);*(i+1)=value.string();
}
inline void run_arguments(Case &t,const std::vector<std::string> &command,const fs::path &directory) {
    LH_CHECK(!command.empty());auto run=t.host(command[0]).cwd(directory);
    for(size_t i=1;i<command.size();++i) run.arg(command[i]);
    t.ok(run);
}
inline fs::path pause_player(Case &t,const fs::path &player,const fs::path &output) {
    const auto build=player.parent_path(),link=build/"CMakeFiles/ff8.dir/link.txt";
    if(!fs::is_regular_file(link)) skip("the release-object pause probe requires the Makefiles CMake generator");
    fs::create_directories(output);
    const auto database=parse_json(read_file(build/"compile_commands.json"));const dt::Json *unit=nullptr;
    for(const auto &row:std::get<dt::Json::Array>(database.value)) {
        auto file=json_string(row.at("file"));const std::string suffix="/backends/host/main.c";
        if(file.size()>=suffix.size() && file.compare(file.size()-suffix.size(),suffix.size(),suffix)==0) {unit=&row;break;}
    }
    LH_CHECK_MSG(unit,"compile_commands.json has no backends/host/main.c unit");std::vector<std::string> command;
    const auto arguments=json_find(*unit,"arguments");
    if(arguments && !std::get<dt::Json::Array>(arguments->value).empty())
        for(const auto &argument:std::get<dt::Json::Array>(arguments->value)) command.push_back(json_string(argument));
    else command=trace::split(json_string(unit->at("command")));
    replace_operand(command,"-c",t.root()/"tests/gui_pause_probe.c");replace_operand(command,"-o",output/"probe.o");
    for(auto flag:{"-MF","-MT","-MQ"}) for(;;) {
        const auto i=std::find(command.begin(),command.end(),flag);if(i==command.end()) break;LH_CHECK(i+1!=command.end());command.erase(i,i+2);
    }
    command.erase(std::remove_if(command.begin(),command.end(),[](const std::string &s) {return s=="-MD" || s=="-MMD" || s=="-MP";}),command.end());
    run_arguments(t,command,json_string(unit->at("directory")));
    command=trace::split(read_file(link));
    command.erase(std::remove_if(command.begin(),command.end(),[](const std::string &s) {return s.rfind("-Wl,--dependency-file=",0)==0;}),command.end());
    const auto binary=output/"pause-player";replace_operand(command,"-o",binary);
    command.insert(command.begin()+1,(output/"probe.o").string());command.push_back("-Wl,--wrap=cpu_init,--wrap=host_gui_open,--wrap=host_gui_frame");
    run_arguments(t,command,build);return binary;
}
}
