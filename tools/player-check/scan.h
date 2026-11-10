// The legal boundary uses exact windows, including data outside code ranges.
#pragma once
#include "json.h"
#include <array>
#include <chrono>
#include <filesystem>
#include <functional>
#include <regex>
#include <set>
#include <string_view>
#include <unordered_set>

namespace player_check {
namespace fs = std::filesystem;
constexpr size_t window_bytes=64, minimum_distinct_bytes=8;
inline std::string lower(std::string s) { for(auto &c:s) c=char(std::tolower(static_cast<unsigned char>(c))); return s; }
inline fs::path input_path(std::string value, const fs::path &directory) {
    require(value.find('\0')==std::string::npos,"embedded null in compiler input path");
    std::replace(value.begin(),value.end(),'\\','/');
    if(value.size()>2 && std::isalpha(static_cast<unsigned char>(value[0])) && value[1]==':' && value[2]=='/') value='/'+value;
    fs::path p=value;
    return fs::weakly_canonical(p.is_absolute()?p:directory/p);
}
// A rolling hash narrows the candidates; memcmp-equivalent comparisons remain
// authoritative, so a collision cannot hide a match or create a finding.
struct Window { uint64_t hash; size_t pos; };
inline std::vector<Window> windows(const std::string &data) {
    std::vector<Window> out; if(data.size()<window_bytes) return out;
    std::array<size_t,256> counts{}; size_t distinct=0; uint64_t hash=0,power=1;
    for(size_t i=1;i<window_bytes;++i) power*=257;
    for(size_t i=0;i<data.size();++i) {
        unsigned char c=data[i]; if(i>=window_bytes) { unsigned char old=data[i-window_bytes];
            hash-=uint64_t(old+1)*power; if(!--counts[old]) --distinct; }
        hash=hash*257+c+1; if(!counts[c]++) ++distinct;
        if(i+1>=window_bytes && distinct>=minimum_distinct_bytes) out.push_back({hash,i+1-window_bytes});
    }
    return out;
}
struct BinaryIndex {
    const std::string &binary;
    std::vector<Window> sorted;
    explicit BinaryIndex(const std::string &b) : binary(b), sorted(windows(b)) {
        std::sort(sorted.begin(),sorted.end(),[](const Window &a,const Window &b){return a.hash<b.hash;});
    }
};
inline Json check_image(const std::string &binary,const std::string &text,const BinaryIndex *index=nullptr) {
    auto guest=windows(text);
    std::unordered_set<std::string_view> unique;
    for(auto &w:guest) unique.emplace(text.data()+w.pos,window_bytes);
    size_t first=std::string::npos;
    if(index) {
        for(auto &w:guest) {
            auto it=std::lower_bound(index->sorted.begin(),index->sorted.end(),w.hash,
                                    [](const Window &v,uint64_t hash){return v.hash<hash;});
            for(;it!=index->sorted.end()&&it->hash==w.hash;++it)
                if(it->pos<first && binary.compare(it->pos,window_bytes,text,w.pos,window_bytes)==0) first=it->pos;
        }
    } else {
        for(size_t pos=0;pos+window_bytes<=binary.size();++pos)
            if(unique.count(std::string_view(binary.data()+pos,window_bytes))) { first=pos; break; }
    }
    require(first==std::string::npos,"guest text window found at binary offset "+std::to_string(first));
    return Json::dict().set("passed",true).set("window_bytes",window_bytes)
        .set("minimum_distinct_bytes",minimum_distinct_bytes).set("unique_guest_windows",unique.size()).set("binary_bytes",binary.size());
}
inline std::vector<std::string> shell_words(const std::string &s) {
    std::vector<std::string> out; std::string word; bool started=false; char quote=0;
    for(size_t i=0;i<s.size();++i) {
        char c=s[i];
        if(!quote && std::isspace(static_cast<unsigned char>(c))) { if(started) { out.push_back(word); word.clear(); started=false; } continue; }
        started=true;
        if(c==quote) { quote=0; continue; }
        if(!quote&&(c=='\''||c=='"')) { quote=c; continue; }
        if(c=='\\'&&quote!='\'') {
            require(i+1<s.size(),"No escaped character"); char next=s[++i];
            if(quote=='"'&&next!='"'&&next!='\\') word+='\\';
            word+=next;
        } else word+=c;
    }
    require(!quote,"No closing quotation"); if(started) out.push_back(word); return out;
}
inline void expand_arguments(const std::vector<std::string> &args,const fs::path &directory,
                             std::set<fs::path> &responses,std::vector<fs::path> &active,std::vector<std::string> &out) {
    for(auto &arg:args) {
        if(!arg.empty()&&arg[0]=='@') {
            auto path=input_path(arg.substr(1),directory);
            require(active.size()<32 && std::find(active.begin(),active.end(),path)==active.end(),"recursive compiler response file: "+path.string());
            std::vector<std::string> nested;
            try { nested=shell_words(read_file(path.string())); }
            catch(const std::exception &e) { throw std::runtime_error("cannot read compiler response file "+path.string()+": "+e.what()); }
            responses.insert(path); active.push_back(path); expand_arguments(nested,directory,responses,active,out); active.pop_back();
        } else if(arg=="-Xclang") continue;
        else if(arg.rfind("-Wp,",0)==0) {
            size_t start=4;
            for(size_t i=4;i<=arg.size();++i) if(i==arg.size()||arg[i]==',') { out.push_back(arg.substr(start,i-start)); start=i+1; }
        } else out.push_back(arg);
    }
}
inline Json check_compile_commands(const fs::path &input,const std::vector<fs::path> &generated={}) {
    auto database=fs::weakly_canonical(fs::absolute(input)); Json units;
    try { units=load_json(database.string()); }
    catch(const std::exception &e) { throw std::runtime_error("cannot read compile commands "+database.string()+": "+e.what()); }
    require(units.kind==Json::array && !units.items.empty(),"compile commands must contain translation units");
    std::vector<fs::path> roots; for(auto &p:generated) roots.push_back(input_path(p.string(),database.parent_path()));
    auto cache=database.parent_path()/"CMakeCache.txt";
    if(fs::is_regular_file(cache)) {
        std::istringstream lines(read_file(cache.string())); std::string line;
        while(std::getline(lines,line)) if(std::regex_search(line,std::regex("^GENERATED_DIR:[^=]+=")))
            roots.push_back(input_path(line.substr(line.find('=')+1),database.parent_path()));
    }
    auto inspect=[&](const std::string &v,const fs::path &directory,const std::string &kind) {
        auto path=input_path(v,directory); bool bad=false;
        for(const auto &part:path.parent_path()) if(lower(part.string()).find("generated")!=std::string::npos) bad=true;
        if(kind=="include" && lower(path.filename().string()).find("generated")!=std::string::npos) bad=true;
        for(auto &root:roots) { auto p=path; for(;;) { if(p==root) bad=true; auto parent=p.parent_path(); if(parent==p) break; p=parent; } }
        if(kind=="source" && std::regex_search(path.filename().string(),std::regex("^(functions_|overlay_|dispatch_table|interpreter_table)"))) bad=true;
        // The player's own module cache, <data>/native/<cache id>/...: code generated from a player's discs.
        std::string previous;
        for(auto &part:path) {
            auto name=lower(part.string());
            if(previous=="native" && name.size()==32 && name.find_first_not_of("0123456789abcdef")==std::string::npos) bad=true;
            previous=name;
        }
        require(!bad,"generated catalog "+kind+" in player compile command: "+path.string());
    };
    std::vector<std::string> options={"-I","-isystem","-iquote","-idirafter","-include","-imacros","-isystem-after","-iframework","-F","--include-directory","/I","/FI","/external:I"};
    std::stable_sort(options.begin(),options.end(),[](const std::string &a,const std::string &b){return a.size()>b.size();});
    std::set<std::string> suffixes={".c",".cc",".cpp",".cxx",".s",".m",".mm"};
    size_t sources=0,includes=0; std::set<fs::path> responses;
    for(auto &unit:units.items) {
        auto file=unit.find("file"); require(unit.kind==Json::object && file && file->kind==Json::string,"compile command is missing its source file");
        auto dir=unit.find("directory"); auto directory=input_path(dir?dir->str():".",database.parent_path());
        inspect(file->str(),directory,"source"); ++sources;
        auto arguments=unit.find("arguments"); std::vector<std::string> args;
        if(!arguments || arguments->kind==Json::null) {
            auto command=unit.find("command"); require(command&&command->kind==Json::string,"compile command needs arguments or command");
            args=shell_words(command->str());
        } else {
            require(arguments->kind==Json::array && !arguments->items.empty(),"compile command arguments must be a nonempty list");
            for(auto &a:arguments->items) { require(a.kind==Json::string,"compile command arguments must be strings"); args.push_back(a.str()); }
        }
        require(!args.empty(),"compile command arguments must be a nonempty list");
        std::vector<std::string> flat; std::vector<fs::path> active; expand_arguments(args,directory,responses,active,flat);
        for(size_t i=1;i<flat.size();++i) {
            auto &a=flat[i]; std::string option;
            for(auto &flag:options) if(a==flag || a.rfind(flag+"=",0)==0 || (flag!="--include-directory" && a.rfind(flag,0)==0 && a.size()>flag.size())) { option=flag; break; }
            if(!option.empty()) {
                auto value=a.substr(option.size()); if(!value.empty()&&value[0]=='=') value.erase(0,1);
                if(value.empty()) { require(++i<flat.size(),"missing path for compiler option "+option); value=flat[i]; }
                inspect(value,directory,"include"); ++includes;
            } else if((a.empty()||a[0]!='-') && suffixes.count(lower(fs::path(a).extension().string()))) { inspect(a,directory,"source"); ++sources; }
        }
    }
    return Json::dict().set("passed",true).set("translation_units",units.items.size()).set("source_paths_checked",sources)
        .set("include_paths_checked",includes).set("response_files_checked",responses.size());
}
struct Image { size_t index; std::string data; };
struct Extraction {
    std::function<std::vector<Image>(const fs::path &,const fs::path &)> legacy;
    std::function<std::vector<Image>(const std::vector<fs::path> &,const fs::path &)> catalog;
};
inline Json check_discs(const std::string &binary,const std::vector<fs::path> &discs,const fs::path &work,const Extraction &extract) {
    fs::create_directories(work);
    auto temporary=work/("scan-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    require(fs::create_directory(temporary),"cannot create scan directory");
    struct Cleanup { fs::path path; ~Cleanup() { std::error_code ec; fs::remove_all(path,ec); } } cleanup{temporary};
    BinaryIndex index(binary); size_t legacy_count=0; Json results=Json::list(),catalog=Json::list();
    auto scan=[&](const Image &image,const std::string &context) {
        Json r;
        try { r=check_image(binary,image.data,&index); }
        catch(const std::exception &e) { throw std::runtime_error(context+": "+e.what()); }
        return Json::dict().set("index",image.index).set("image_bytes",image.data.size()).set("unique_guest_windows",r.at("unique_guest_windows"));
    };
    for(size_t i=0;i<discs.size();++i) {
        Json rows=Json::list(); auto images=extract.legacy(discs[i],temporary/"legacy"/std::to_string(i));
        for(auto &image:images) rows.items.push_back(scan(image,discs[i].string()+", overlay "+std::to_string(image.index)));
        require(!rows.items.empty(),discs[i].string()+": extraction produced no overlay images");
        legacy_count+=rows.items.size(); results.items.push_back(Json::dict().set("disc",discs[i].string()).set("overlays",std::move(rows)));
    }
    for(auto &image:extract.catalog(discs,temporary/"catalog")) catalog.items.push_back(scan(image,"catalog overlay "+std::to_string(image.index)));
    require(!catalog.items.empty(),"default recompiler catalog extraction produced no overlay images");
    auto total=catalog.items.size();
    return Json::dict().set("passed",true).set("window_bytes",window_bytes).set("minimum_distinct_bytes",minimum_distinct_bytes)
        .set("discs",std::move(results)).set("catalog",std::move(catalog)).set("legacy_full_images",legacy_count)
        .set("catalog_loaded_images",total).set("overlay_images",legacy_count+total);
}
inline uint32_t u32(const std::string &s,size_t p) {
    require(p<=s.size() && s.size()-p>=4,"truncated image"); const auto *b=reinterpret_cast<const unsigned char *>(s.data()+p);
    return uint32_t(b[0]) | uint32_t(b[1])<<8 | uint32_t(b[2])<<16 | uint32_t(b[3])<<24;
}
inline Json scan_cli(const std::vector<std::string> &args,const Extraction &extract,const fs::path &root) {
    std::vector<fs::path> discs,generated; std::vector<std::string> positional;
    fs::path database,work=root/"build/host/legal-extracted"; bool literal=false;
    for(size_t i=0;i<args.size();++i) {
        if(!literal && args[i]=="--") { literal=true; continue; }
        if(literal || args[i].empty() || args[i][0]!='-' || args[i]=="-") { positional.push_back(args[i]); continue; }
        auto equals=args[i].find('='); auto option=args[i].substr(0,equals); std::string value;
        require(option=="--disc" || option=="--compile-commands" || option=="--generated-dir" || option=="--extract-dir","unknown option: "+option);
        if(equals!=std::string::npos) value=args[i].substr(equals+1);
        else { require(i+1<args.size(),"missing value for "+option); value=args[++i]; }
        if(option=="--disc") discs.emplace_back(value);
        else if(option=="--compile-commands") database=value;
        else if(option=="--generated-dir") generated.emplace_back(value);
        else work=value;
    }
    require(positional.size()==2,"usage: check_player_binary BINARY EXE [--disc CUE] [--compile-commands PATH] [--generated-dir PATH] [--extract-dir PATH]");
    auto binary=read_file(positional[0]),exe=read_file(positional[1]);
    require(exe.size()>=2048 && exe.compare(0,8,"PS-X EXE")==0,"not a PS-X EXE");
    auto size=u32(exe,28); require(size<=exe.size()-2048,"truncated PS-X EXE");
    Json result=check_image(binary,exe.substr(2048,size));
    if(!discs.empty()) result.set("overlays",check_discs(binary,discs,work,extract));
    if(!database.empty()) result.set("compile_commands",check_compile_commands(database,generated));
    return result;
}
} // namespace player_check
