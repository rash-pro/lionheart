#pragma once
#include "compare.h"
#include <chrono>
extern char **environ;
namespace bench {
using namespace comparison;
inline Json distribution(std::vector<double> v) {
    iro::require(!v.empty(),"empty timing window");std::sort(v.begin(),v.end());Json out=Json::object();
    for(int p:{50,95,99}) {
        // Python rounds ties to the even index, independently of the FPU mode.
        double index=(v.size()-1)*p/100.0;size_t n=size_t(index);double frac=index-n;
        if(frac>0.5||(frac==0.5&&(n&1))) ++n;
        out.set("p"+std::to_string(p),v[n]);
    }
    out.set("max",v.back());out.set("above_16_7",int64_t(std::count_if(v.begin(),v.end(),[](double x){return x>16.7;})));out.set("above_33",int64_t(std::count_if(v.begin(),v.end(),[](double x){return x>33;})));return out;
}
struct Args {fs::path card,exe,script;std::vector<fs::path> discs;std::string renderer="soft";bool interpreter=false;int first=2100,last=5400;};
inline std::string load() {auto s=dt::read("/proc/loadavg");while(!s.empty()&&isspace(s.back()))s.pop_back();return s;}
inline Json measure(const fs::path &binary,const Args &a,fs::path dest,const fs::path &checkpoint={}) {
    dest=fs::weakly_canonical(dest);iro::require(fs::create_directories(dest),"output already exists");
    auto card=dest/"card.mcr",config=dest/"empty.ini";auto original=dt::sha256_file(a.card);fs::copy_file(a.card,card);dt::write(config,"");
    auto abs=[](const fs::path &p){return fs::weakly_canonical(p).string();};
    std::vector<std::string> cmd={abs(binary),abs(a.exe),"--config",config.string(),"--headless","--fast","--mute","--renderer",a.renderer,"--stall","0","--max-instructions","0","--max-vblanks",std::to_string(a.last),"--input-script",abs(a.script),"--card1",card.string(),"--savestate-dir",(dest/"states").string(),"--screenshot-dir",(dest/"shots").string(),"--log","/dev/null"};
    for(const auto &disc:a.discs) {cmd.push_back("--disc");cmd.push_back(abs(disc));}if(a.interpreter)cmd.push_back("--interpreter");if(!checkpoint.empty()){cmd.push_back("--load-state");cmd.push_back(abs(checkpoint));}
    Json r=Json::Object{{"binary_sha256",dt::sha256_file(binary)},{"source_card_sha256",original},{"script_sha256",dt::sha256_file(a.script)},{"renderer",a.renderer},{"interpreter",a.interpreter},{"first",a.first},{"last",a.last},{"load_before",load()}};
    if(!checkpoint.empty())r.set("checkpoint_sha256",dt::sha256_file(checkpoint));
    // Isolate the child's FF8 environment without changing the caller's state.
    std::vector<std::pair<std::string,std::string>> saved;
    for(char **p=environ;*p;++p) {std::string s=*p;auto n=s.find('=');if(s.rfind("FF8_",0)==0)saved.emplace_back(s.substr(0,n),s.substr(n+1));}
    for(const auto &p:saved)unsetenv(p.first.c_str());
    auto start=std::chrono::steady_clock::now();int rc;
    try {rc=dt::run(cmd,{},dest/"run.log",{{"SDL_VIDEODRIVER","dummy"},{"SDL_AUDIODRIVER","dummy"},{"XDG_CONFIG_HOME",(dest/"config").string()},{"XDG_DATA_HOME",(dest/"data").string()},{"FF8_FRAME_TIMINGS",(dest/"frames.csv").string()},{"BATTLE_TIMINGS",(dest/"frames.csv").string()}});}catch(...){for(const auto &p:saved)setenv(p.first.c_str(),p.second.c_str(),1);throw;}
    for(const auto &p:saved)setenv(p.first.c_str(),p.second.c_str(),1);
    r.set("elapsed_seconds",std::chrono::duration<double>(std::chrono::steady_clock::now()-start).count());r.set("exit_code",rc);r.set("load_after",load());auto log=dt::read(dest/"run.log");
    bool complete=rc==0&&log.find("Stop: VBlank limit reached")!=std::string::npos&&log.find("VBlanks="+std::to_string(a.last)+" ")!=std::string::npos;bool unchanged=dt::sha256_file(a.card)==original;r.set("complete",complete);r.set("source_card_unchanged",unchanged);
    if(!complete||!unchanged){dt::write(dest/"receipt.json",r.dump(2)+"\n");throw std::runtime_error("incomplete or modified input: "+dest.string());}
    if(a.renderer=="vulkan") {std::smatch m;iro::require(std::regex_search(log,m,std::regex("(?:^|\\n)Vulkan: (.+), scale="))&&log.find("falling back to soft")==std::string::npos,("requested Vulkan renderer was not used: "+dest.string()).c_str());r.set("vulkan_device",m[1].str());}
    std::istringstream in(dt::read(dest/"frames.csv"));std::string line;std::getline(in,line);std::vector<std::string> keys;std::istringstream header(line);while(std::getline(header,line,','))keys.push_back(line);
    std::vector<std::map<std::string,std::string>> rows;while(std::getline(in,line)){if(line.empty())continue;std::istringstream row(line);std::map<std::string,std::string> values;size_t i=0;while(std::getline(row,line,',')){iro::require(i<keys.size(),"invalid timing row");values[keys[i++]]=line;}rows.push_back(values);}
    int frame=!checkpoint.empty()&&!rows.empty()?std::stoi(rows[0].at("vblank")):1;
    iro::require(checkpoint.empty()||frame<a.first,"missing or repeated VBlank samples");for(const auto &row:rows)iro::require(std::stoi(row.at("vblank"))==frame++,"missing or repeated VBlank samples");iro::require(frame==a.last+1,"missing or repeated VBlank samples");
    Json work=Json::array();std::vector<double> cpu,wall;for(const auto &row:rows)if(std::stoi(row.at("vblank"))>=a.first){Json v=Json::array();for(const char *key:{"vblank","cycles","interpreted","compiled","idle","gte","cd_bytes"})v.push(int64_t(std::stoll(row.at(key))));work.push(v);cpu.push_back(std::stod(row.at("cpu_ms")));wall.push_back(std::stod(row.at("wall_ms")));}
    r.set("work_sha256",dt::sha256(work.dump()));std::smatch m;iro::require(std::regex_search(log,m,std::regex("Hardware: cycles=([0-9]+)")),"missing hardware cycles");r.set("cycles",int64_t(std::stoll(m[1])));r.set("frames",int64_t(cpu.size()));r.set("cpu_ms",distribution(cpu));r.set("wall_ms",distribution(wall));dt::write(dest/"receipt.json",r.dump(2)+"\n");return r;
}
}
