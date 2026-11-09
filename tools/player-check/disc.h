// Read owned discs in place. The scanner never needs generated guest code.
#pragma once
#include "scan.h"
#include <iomanip>
#include <map>
#include <memory>

extern "C" void player_check_sha1(const void *bytes,size_t size,char out[41]);
namespace player_check {
inline std::string sha1(const std::string &data) { char hash[41]; player_check_sha1(data.data(),data.size(),hash); return hash; }
inline std::string lzs(const std::string &raw) {
    size_t end=std::min(raw.size(),size_t(u32(raw,0))+4),p=4,r=0xfee;
    std::array<char,4096> ring{}; std::string out;
    auto emit=[&](char c) { require(out.size()<256*1024*1024,"decompressed image too large"); out+=c; ring[r]=c; r=(r+1)&4095; };
    while(p<end) {
        unsigned flags=static_cast<unsigned char>(raw[p++]);
        for(unsigned bit=0;bit<8&&p<end;++bit) {
            if(flags>>bit&1) emit(raw[p++]);
            else {
                if(p+1>=end) { p=end; break; }
                unsigned a=static_cast<unsigned char>(raw[p++]),b=static_cast<unsigned char>(raw[p++]);
                size_t offset=a|((b&0xf0)<<4),length=(b&15)+3;
                for(size_t k=0;k<length;++k) emit(ring[(offset+k)&4095]);
            }
        }
    }
    return out;
}
struct Record { size_t lba,size; bool directory; std::string name; };
class Disc {
    std::ifstream bin;
    size_t bytes;
    static Record record(const std::string &s,size_t p) {
        require(p<s.size() && static_cast<unsigned char>(s[p])>=34 && p+static_cast<unsigned char>(s[p])<=s.size(),"invalid ISO record");
        size_t n=static_cast<unsigned char>(s[p+32]); require(33+n<=static_cast<unsigned char>(s[p]),"invalid ISO name");
        auto name=s.substr(p+33,n); name=name.substr(0,name.find(';'));
        return {u32(s,p+2),u32(s,p+10),bool(s[p+25]&2),name};
    }
    void walk(const Record &dir,const std::string &prefix,unsigned depth) {
        require(depth<32,"recursive ISO directory"); auto data=read(dir.lba,dir.size);
        for(size_t pos=0;pos<data.size();) {
            unsigned n=static_cast<unsigned char>(data[pos]); if(!n) { pos=(pos/2048+1)*2048; continue; }
            auto rec=record(data,pos); pos+=n;
            if(rec.name==std::string(1,'\0') || rec.name==std::string(1,'\1')) continue;
            auto path=prefix+rec.name; files.emplace_back(path,rec);
            if(rec.directory) walk(rec,path+'/',depth+1);
        }
    }
public:
    std::vector<std::pair<std::string,Record>> files;
    explicit Disc(const fs::path &cue) {
        std::istringstream lines(read_file(cue.string())); std::string line; fs::path image;
        while(std::getline(lines,line)) {
            size_t p=line.find_first_not_of(" \t\r");
            if(p!=std::string::npos && line.compare(p,4,"FILE")==0) {
                size_t first=line.find('"'),last=line.find('"',first+1);
                require(first!=std::string::npos&&last!=std::string::npos,"invalid CUE FILE"); image=cue.parent_path()/line.substr(first+1,last-first-1); break;
            }
        }
        require(!image.empty(),"missing CUE FILE");
        bin.open(image,std::ios::binary); require(bool(bin),"cannot read disc image");
        bin.seekg(0,std::ios::end); auto n=bin.tellg(); require(n>=0,"cannot size disc image"); bytes=size_t(n);
        auto pvd=read(16,2048); walk(record(pvd,156),"",0);
    }
    std::string read(size_t lba,size_t size) {
        require(size<=256*1024*1024 && lba<=bytes/2352 && (size+2047)/2048<=bytes/2352-lba,"truncated disc image");
        std::string out(size,'\0');
        for(size_t at=0;at<size;at+=2048,++lba) {
            bin.clear(); bin.seekg(std::streamoff(lba*2352+24)); size_t n=std::min(size-at,size_t(2048));
            bin.read(&out[at],std::streamsize(n)); require(size_t(bin.gcount())==n,"truncated disc sector");
        }
        return out;
    }
    const Record &find(const std::string &path) const {
        for(auto &file:files) if(file.first==path) return file.second;
        throw std::runtime_error("missing disc file: "+path);
    }
    const Record &find_last(const std::string &path) const {
        for(auto it=files.rbegin();it!=files.rend();++it) if(it->first==path) return it->second;
        throw std::runtime_error("missing disc file: "+path);
    }
    size_t length() const { return bytes; }
};
inline void verify_exe(const std::string &exe,const Json &metadata) {
    require(sha1(exe)==metadata.at("exe_sha1").str(),"unsupported disc EXE SHA-1");
    require(exe.size()>=2048&&exe.compare(0,8,"PS-X EXE")==0,"not a PS-X EXE");
    uint32_t entry=u32(exe,16),base=u32(exe,24),size=u32(exe,28);
    require(size<=exe.size()-2048 && uint64_t(base&0x1fffffff)+size<=0x200000,"truncated EXE or text outside RAM");
    require(!(base&3)&&!(size&3)&&!(entry&3)&&entry>=base&&uint64_t(entry)<uint64_t(base)+size,"unaligned EXE or entry outside text");
    auto entries=metadata.find("main_entry_points");
    if(entries) for(auto &pc:entries->values()) {
        auto p=std::stoull(pc.str(),nullptr,16); bool in=false;
        auto &ranges=metadata.at("main_code_ranges").values();
        for(auto &range:ranges) if(p>=std::stoull(range.items.at(0).str(),nullptr,16)&&p<std::stoull(range.items.at(1).str(),nullptr,16)) in=true;
        require(!(p&3)&&p>=base&&p+4<=uint64_t(base)+size&&(ranges.empty()||in),"main entry point outside executable ranges");
    }
}
inline bool decodable(uint32_t word) {
    unsigned op=word>>26,rs=word>>21&31,fn=word&63;
    if(op==0) { static const std::set<unsigned> special={0,2,3,4,6,7,8,9,12,13,16,17,18,19,24,25,26,27,32,33,34,35,36,37,38,39,42,43}; return special.count(fn); }
    if(op==1) return true;
    if(op==16||op==18) return rs==0||rs==2||rs==4||rs==6||rs==8||(rs>=16&&(op==18||fn==16));
    static const std::set<unsigned> primary={2,3,4,5,6,7,8,9,10,11,12,13,14,15,32,33,34,35,36,37,38,40,41,42,43,46,50,58};
    return primary.count(op);
}
inline void verify_effect_base(const std::string &data) {
    std::vector<size_t> votes;
    for(uint32_t base:{0x80190000u,0x801a0000u,0x801b0000u}) {
        size_t count=0;
        for(size_t offset=0;offset+4<=std::min(data.size(),size_t(4096));offset+=4) {
            auto word=u32(data,offset); if(word>>26!=2&&word>>26!=3) continue;
            uint32_t target=0x80000000u|((word&0x3ffffff)<<2);
            if(target<base || size_t(target-base)+4>data.size()) continue;
            size_t dest=target-base; auto ins=u32(data,dest);
            bool trampoline=offset==0 && ins>>16==0x27bd && (ins&0x8000);
            if((dest>offset?dest-offset:offset-dest)>=0x4000&&!trampoline) continue;
            if(ins&&decodable(ins)) ++count;
        }
        votes.push_back(count);
    }
    std::sort(votes.begin(),votes.end(),std::greater<size_t>());
    require(votes[0] && votes[0]!=votes[1],"effect load address is ambiguous");
}
inline std::vector<Image> legacy_images(const fs::path &cue,const fs::path &root) {
    auto metadata=load_json((root/"games/ff8-usa/overlays.json").string()); Disc disc(cue);
    const Json *info=nullptr; size_t number=0,matches=0;
    for(auto &row:metadata.at("discs").members) {
        bool exe=false,img=false;
        for(auto &f:disc.files) { if(f.first==row.second.at("exe").str()) exe=true; if(f.first==row.second.at("image").str()) img=true; }
        if(exe&&img) { ++matches; number=std::stoul(row.first); info=&row.second; }
    }
    require(matches==1,"disc does not identify one supported FF8 USA disc");
    auto exe=disc.find_last(info->at("exe").str()); verify_exe(disc.read(exe.lba,exe.size),metadata);
    auto img=disc.find_last(info->at("image").str()); auto table=disc.read(img.lba,2048); std::vector<Image> result;
    for(auto &row:info->at("overlays").values()) {
        size_t index=row.at("index").integer(),lba,size;
        if(auto sector=row.find("img_sector")) { lba=img.lba+sector->integer(); size=row.at("stored_size").integer(); }
        else { lba=u32(table,index*8); size=u32(table,index*8+4); }
        auto data=disc.read(lba,size); if(row.at("compressed").truth()) data=lzs(data);
        auto address=row.at("load_address").str(); size_t end=0; auto base=std::stoull(address,&end,16);
        require(end==address.size(),"invalid overlay load address");
        std::ostringstream formatted; formatted<<"0x"<<std::hex<<std::setw(8)<<std::setfill('0')<<base;
        require(data.size()==row.at("size").integer() && sha1(data)==row.at("sha1").str() && formatted.str()==address,
                "overlay "+std::to_string(index)+" does not match versioned metadata");
        result.push_back({(number-1)*32+index,std::move(data)});
    }
    return result;
}
inline std::vector<Image> catalog_images(const std::vector<fs::path> &cues,const fs::path &root) {
    auto directory=root/"games/ff8-usa";
    auto metadata=load_json((directory/"overlays.json").string()); auto catalog=load_json((directory/"overlays-all.json").string());
    struct Media { std::unique_ptr<Disc> disc; Record image; Json inventory; };
    std::map<size_t,Media> discs; std::string main;
    for(auto &cue:cues) {
        auto disc=std::make_unique<Disc>(cue); size_t number=0,count=0; Record image{};
        for(auto &file:disc->files) if(std::regex_match(file.first,std::regex("FF8DISC[1-4]\\.IMG"))) { number=size_t(file.first[7]-'0'); image=file.second; ++count; }
        require(count==1,"expected exactly one FF8DISC[1-4].IMG");
        require(!discs.count(number),"the same disc was supplied more than once");
        require(image.size%2048==0,"IMG size is not sector aligned");
        require(image.lba<=disc->length()/2352 && image.size/2048<=disc->length()/2352-image.lba,"truncated disc image");
        auto table=disc->read(image.lba,2048); size_t tops=0;
        for(size_t p=0;p+8<=table.size();p+=8) { if(u32(table,p)==0&&u32(table,p+4)==0) break; ++tops; }
        require(tops==133,"expected 133 top-table entries, got "+std::to_string(tops));
        auto inv=load_json((directory/("files-disc"+std::to_string(number)+".json")).string());
        require(image.lba==inv.at("image").at("lba").integer() && image.size==inv.at("image").at("size").integer(),"disc IMG does not match inventory layout");
        auto exerec=disc->find(inv.at("exe").at("name").str()); auto exe=disc->read(exerec.lba,exerec.size);
        require(main.empty()||main==exe,"disc EXEs differ"); main=exe; verify_exe(exe,metadata);
        discs.emplace(number,Media{std::move(disc),image,std::move(inv)});
    }
    std::vector<Image> result; size_t extra=128;
    std::map<std::pair<std::string,size_t>,std::pair<std::string,std::string>> decoded;
    for(auto &group:catalog.at("overlays").values()) {
        struct Loaded { std::string hash,data; Json location; };
        std::vector<Loaded> verified;
        for(auto &loc:group.at("locations").values()) {
            size_t number=loc.at("disc").integer(); auto it=discs.find(number); if(it==discs.end()) continue;
            auto &media=it->second; size_t sector=loc.at("sector").integer(),size=loc.at("size").integer(),read_size=loc.at("read_size").integer();
            require(sector<=media.image.size/2048 && size<=media.image.size-sector*2048,"IMG read outside image");
            auto raw=media.disc->read(media.image.lba+sector,size);
            auto key=std::make_pair(loc.at("sha1_stored").str(),read_size); auto cached=decoded.find(key);
            if(cached==decoded.end()) {
                auto data=loc.at("compressed").truth()?lzs(raw):raw;
                auto loaded=loc.at("compressed").truth()&&read_size<raw.size()?lzs(raw.substr(0,read_size)):data;
                cached=decoded.emplace(key,std::make_pair(std::move(data),std::move(loaded))).first;
            }
            auto &data=cached->second.first; auto &loaded=cached->second.second;
            require(sha1(raw)==loc.at("sha1_stored").str() && data.size()==group.at("size").integer()&&sha1(data)==group.at("sha1").str(),"catalog bytes disagree: "+loc.at("path").str()+" disc "+std::to_string(number));
            auto hash=loc.at("loaded_sha1").str();
            require(loaded.size()==loc.at("loaded_size").integer()&&sha1(loaded)==hash,"loader bytes disagree: "+loc.at("path").str());
            auto slot=std::find_if(verified.begin(),verified.end(),[&](const Loaded &v){return v.hash==hash;});
            Loaded value{hash,loaded,loc}; if(slot==verified.end()) verified.push_back(std::move(value)); else *slot=std::move(value);
        }
        for(auto &v:verified) {
            auto &address=v.location.at("load_address");
            if(address.kind==Json::null || address.text.empty()) {
                verify_effect_base(decoded.at({v.location.at("sha1_stored").str(),v.location.at("read_size").integer()}).first);
            } else {
                auto text=address.str(); size_t end=0; (void)std::stoull(text,&end,16);
                require(end==text.size(),"invalid catalog load address");
            }
            auto &inv=discs.at(v.location.at("disc").integer()).inventory; const Json *locator=nullptr;
            for(auto &entry:inv.at("entries").values()) if(entry.at("path").str()==v.location.at("path").str()) { locator=&entry.at("locator"); break; }
            require(locator,"catalog location missing from inventory"); size_t index=extra;
            if(locator->at("table").str()=="top") {
                size_t top=locator->at("index").integer();
                if(top<=2 || (top>=4&&top<=20) || (top>=23&&top<=26)) index=(v.location.at("disc").integer()-1)*32+top;
            }
            if(index==extra) ++extra;
            result.push_back({index,std::move(v.data)});
        }
    }
    std::sort(result.begin(),result.end(),[](const Image &a,const Image &b){return a.index<b.index;});
    for(size_t i=1;i<result.size();++i) require(result[i-1].index!=result[i].index,"catalog set index collision");
    return result;
}
inline Extraction disc_extraction(const fs::path &root) {
    return {[root](const fs::path &cue,const fs::path &){return legacy_images(cue,root);},
            [root](const std::vector<fs::path> &cues,const fs::path &){return catalog_images(cues,root);}};
}
} // namespace player_check
