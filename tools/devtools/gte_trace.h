#pragma once
#include "frame_probe.h"
namespace trace {
inline std::vector<std::string> flags(const std::string &text,const std::string &key) {
    std::istringstream in(text);std::string line;
    while(std::getline(in,line)) if(line.rfind(key+" = ",0)==0) return split(line.substr(key.size()+3));
    return {};
}
inline fs::path gte_replay(const fs::path &root,fs::path dest,const std::string &cc,const std::vector<std::string> &extra={}) {
    dest=fs::weakly_canonical(dest);fs::create_directories(dest);
    std::vector<std::string> common={cc,"-std=c99","-O2","-Wall","-Wextra","-Werror","-I",(root/"runtime").string()};append(common,extra);
    if(getenv("FF8_TEST_SANITIZE")&&*getenv("FF8_TEST_SANITIZE")) append(common,{"-fsanitize=undefined","-fno-sanitize-recover=all"});
    for(bool ref:{false,true}) {
        auto cmd=common;
        if(ref) append(cmd,{"-DFF8_GTE_REFERENCE","-Dgte_cmd=gte_reference_cmd","-Dgte_read=gte_reference_read","-Dgte_write=gte_reference_write"});
        append(cmd,{"-c",(root/"runtime/gte.c").string(),"-o",(dest/(ref?"reference.o":"gte.o")).string()});checked(cmd);
    }
    auto cmd=common;append(cmd,{(root/"tests/gte_diff.c").string(),(dest/"gte.o").string(),(dest/"reference.o").string(),"-o",(dest/"replay").string()});checked(cmd);return dest/"replay";
}
inline fs::path gte_record(const fs::path &root,fs::path host,fs::path dest,const std::string &cc) {
    host=fs::weakly_canonical(host);dest=fs::weakly_canonical(dest);fs::create_directories(dest);
    iro::require(dt::run({"nm","--defined-only",(host/"ff8").string()}, {},dest/"symbols.txt")==0,"nm failed");
    bool specialized=dt::read(dest/"symbols.txt").find(" gte_rtps\n")!=std::string::npos;fs::remove(dest/"symbols.txt");
    std::vector<std::string> symbols={"gte_cmd","host_present"};
    if(specialized) for(const auto &p:gte_declarations(root)) symbols.push_back(p.first);
    auto text=dt::read(host/"CMakeFiles/ff8.dir/flags.make");
    std::vector<std::string> cmd={cc,"-DFF8_GTE_CAPTURE_FRAMES"};
    if(specialized) cmd.push_back("-DFF8_GTE_SPECIALIZED_TRACE");
    for(const char *key:{"C_DEFINES","C_INCLUDES","C_FLAGS"}) append(cmd,flags(text,key));
    append(cmd,{"-c",(root/"tools/gte_trace.c").string(),"-o",(dest/"trace.o").string()});checked(cmd);
    cmd=split(dt::read(host/"CMakeFiles/ff8.dir/link.txt"));output(cmd,dest/"record");cmd.push_back((dest/"trace.o").string());
    std::string wrap="-Wl";for(const auto &s:symbols) wrap+=",--wrap="+s;cmd.push_back(wrap);checked(cmd,host);return dest/"record";
}
}
