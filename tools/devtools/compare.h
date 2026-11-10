#pragma once
#include "hash.h"
#include "process.h"
#include <regex>
namespace comparison {
namespace fs=std::filesystem;
using dt::Json;
inline Json run(const fs::path &binary,const fs::path &exe,const std::vector<fs::path> &discs,const fs::path &script,int count,fs::path dest,const fs::path &card={},const std::vector<std::string> &launcher={},const std::vector<std::string> &extra={}) {
    dest=fs::weakly_canonical(dest); fs::create_directories(dest);
    auto config=dest/"empty.ini",private_card=dest/"card.mcr"; dt::write(config,"");
    iro::require(card.empty() || fs::weakly_canonical(card)!=fs::weakly_canonical(private_card),"input card must be outside the comparison output directory");
    if(fs::exists(private_card)) fs::remove(private_card);
    std::string original=card.empty()?"":dt::sha256_file(card);
    if(!card.empty()) fs::copy_file(card,private_card);
    auto abs=[](const fs::path &p){return fs::weakly_canonical(p).string();};
    std::vector<std::string> command=launcher;
    std::vector<std::string> args={abs(binary),abs(exe),"--config",config.string(),"--headless","--fast","--mute","--renderer","soft","--stall","0","--max-instructions","0","--max-vblanks",std::to_string(count),"--input-script",abs(script),"--card1",private_card.string(),"--savestate-dir",(dest/"states").string(),"--screenshot-dir",(dest/"shots").string(),"--dump-state",(dest/"state.bin").string(),"--dump-frames",(dest/"frames").string(),"--frame-step","1","--log",launcher.empty()?"/dev/null":"NUL"};
    command.insert(command.end(),args.begin(),args.end());
    for(const auto &disc:discs) {command.push_back("--disc");command.push_back(abs(disc));}
    command.insert(command.end(),extra.begin(),extra.end());
    auto rc=dt::run(command,dest,dest/"run.log",{{"SDL_VIDEODRIVER","dummy"},{"SDL_AUDIODRIVER","dummy"},{"XDG_CONFIG_HOME",(dest/"config").string()},{"XDG_DATA_HOME",(dest/"data").string()}});
    auto text=dt::read(dest/"run.log");
    if(rc || text.find("Stop: VBlank limit reached")==std::string::npos || text.find("VBlanks="+std::to_string(count)+" ")==std::string::npos) throw std::runtime_error("incomplete run: "+(dest/"run.log").string());
    std::vector<fs::path> frames;
    if(fs::is_directory(dest/"frames")) for(const auto &entry:fs::directory_iterator(dest/"frames")) if(entry.path().extension()==".png") frames.push_back(entry.path());
    std::sort(frames.begin(),frames.end());
    if(int(frames.size())!=count) throw std::runtime_error("expected "+std::to_string(count)+" frames, got "+std::to_string(frames.size()));
    Json cmds=Json::array(),hashes=Json::object(); for(const auto &s:command) cmds.push(s);
    for(const auto &p:frames) hashes.set(p.filename().string(),dt::sha256_file(p));
    std::smatch match; iro::require(std::regex_search(text,match,std::regex("Hardware: cycles=([0-9]+)")),"missing hardware cycles");
    Json receipt=Json::Object{{"command",cmds},{"binary_sha256",dt::sha256_file(binary)},{"state",dt::sha256_file(dest/"state.bin")},{"frames",hashes},{"card",dt::sha256_file(private_card)},{"source_card_unchanged",card.empty()||dt::sha256_file(card)==original},{"cycles",int64_t(std::stoll(match[1]))}};
    iro::require(std::get<bool>(receipt.at("source_card_unchanged").value),"input memory card changed");
    dt::write(dest/"receipt.json",receipt.dump(2)+"\n");
    for(size_t i=0;i+1<frames.size();++i) fs::remove(frames[i]);
    return receipt;
}
inline Json compare(const Json &before,const Json &after) {
    Json result=Json::Object{{"passed",true}};
    for(const char *key:{"state","frames","card","cycles"}) {
        bool equal=before.at(key).dump()==after.at(key).dump();
        iro::require(equal,"exactness mismatch"); result.set(key,equal);
    }
    result.set("pngs",int64_t(std::get<Json::Object>(before.at("frames").value).size())); return result;
}
}
