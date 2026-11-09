#pragma once
#include "json.h"
#include <regex>
namespace perf {
using dt::Json;
inline Json summarize(const std::string &log,int first,int last,const std::filesystem::path &histogram={}) {
    using Counters=std::vector<std::pair<std::string,int64_t>>;
    using Key=std::pair<std::string,int>;
    std::map<int,Counters> counters;
    std::map<int,std::vector<std::pair<Key,int64_t>>> candidates;
    const std::regex field("(\\w+)=(\\w+)");
    std::istringstream lines(log); std::string line;
    while(std::getline(lines,line)) {
        auto pos=line.find("CPU "); if(pos!=std::string::npos) line=line.substr(pos);
        if(line.rfind("CPU MEMORY ",0) && line.rfind("CPU DISPATCH ",0) && line.rfind("CPU CANDIDATE ",0)) continue;
        std::vector<std::pair<std::string,std::string>> fields;
        std::map<std::string,std::string> lookup;
        for(auto i=std::sregex_iterator(line.begin(),line.end(),field);i!=std::sregex_iterator();++i) {
            auto k=(*i)[1].str(),v=(*i)[2].str(); fields.emplace_back(k,v); lookup[k]=v;
        }
        int vb=std::stoi(lookup.at("vb")); if(vb!=first && vb!=last) continue;
        if(line.rfind("CPU CANDIDATE ",0)==0) {
            Key key{lookup.at("pc"),std::stoi(lookup.at("kind"))}; auto &rows=candidates[vb]; bool found=false;
            for(auto &r:rows) if(r.first==key) { r.second=std::stoll(lookup.at("hits")); found=true; break; }
            if(!found) rows.emplace_back(key,std::stoll(lookup.at("hits")));
        } else {
            auto group=line.substr(4,line.find(' ',4)-4); group=iro::lower(group);
            for(const auto &p:fields) if(p.first!="vb") {
                auto key=group+"."+p.first; auto &rows=counters[vb]; bool found=false;
                for(auto &r:rows) if(r.first==key) { r.second=std::stoll(p.second); found=true; break; }
                if(!found) rows.emplace_back(key,std::stoll(p.second));
            }
        }
    }
    iro::require(counters.count(first)&&counters.count(last),"missing exact boundary snapshots");
    std::map<std::string,int64_t> before(counters[first].begin(),counters[first].end());
    Json delta=Json::object(); int64_t lost=0;
    for(const auto &p:counters[last]) { auto n=p.second-before.at(p.first); iro::require(n>=0,"counters decreased"); delta.set(p.first,n); if(p.first=="dispatch.candidate_lost") lost=n; }
    auto hits=candidates[last]; std::map<Key,int64_t> initial(candidates[first].begin(),candidates[first].end());
    for(auto &p:hits) p.second-=initial[p.first];
    std::stable_sort(hits.begin(),hits.end(),[](const auto &a,const auto &b){return a.second>b.second;});
    Json rows=Json::array(); for(const auto &p:hits) if(p.second) rows.push(Json::Object{{"pc",p.first.first},{"kind",p.first.second},{"hits",p.second}});
    Json result=Json::Object{{"first_vblank",first},{"last_vblank",last},{"counters",delta},{"candidates_complete",!lost},{"candidates",rows}};
    if(!histogram.empty()) {
        std::vector<std::pair<uint32_t,int64_t>> pcs; std::istringstream csv(dt::read(histogram)); std::getline(csv,line);
        while(std::getline(csv,line)) { auto comma=line.find(','); if(comma==std::string::npos) continue; auto pc=uint32_t(std::stoul(line.substr(0,comma),nullptr,16)); auto n=std::stoll(line.substr(comma+1)); bool found=false; for(auto &p:pcs) if(p.first==pc) {p.second=n;found=true;} if(!found) pcs.emplace_back(pc,n); }
        std::map<uint32_t,int64_t> counts(pcs.begin(),pcs.end()); int64_t total=0,battle=0; for(const auto &p:pcs) total+=p.second;
        iro::require(total!=0,"empty PC histogram"); Json battle_pcs=Json::object();
        auto hex=[](uint32_t pc) { std::ostringstream s; s<<std::hex<<std::setw(8)<<std::setfill('0')<<pc; return s.str(); };
        for(uint32_t start:{0x800d073cu,0x800cf3fcu}) for(uint32_t pc=start;pc<start+16;pc+=4) { battle+=counts[pc]; battle_pcs.set(hex(pc),counts[pc]); }
        result.set("instructions",total).set("battle_poll",Json::Object{{"instructions",battle},{"percent",100.0*battle/total},{"pcs",battle_pcs}}).set("world_bf374",counts[0x800bf374]);
        std::stable_sort(pcs.begin(),pcs.end(),[](const auto &a,const auto &b){return a.second>b.second;}); Json hot=Json::array();
        for(size_t i=0;i<std::min<size_t>(40,pcs.size());++i) hot.push(Json::Object{{"pc",hex(pcs[i].first)},{"instructions",pcs[i].second},{"percent",100.0*pcs[i].second/total}});
        result.set("hot_pcs",hot);
    }
    return result;
}
}
