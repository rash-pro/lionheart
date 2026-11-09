#pragma once
#include "process.h"
#include <regex>
namespace trace {
namespace fs=std::filesystem;
inline std::vector<std::string> split(const std::string &text) {
    std::vector<std::string> result; std::string word; char quote=0; bool token=false;
    for(size_t i=0;i<text.size();++i) {
        char c=text[i];
        if(c=='\\' && quote!='\'') {
            iro::require(i+1<text.size(),"unfinished shell escape"); char next=text[++i];
            if(quote=='"' && next!='"' && next!='\\' && next!='$' && next!='`' && next!='\n') word+='\\';
            if(next!='\n') word+=next; token=true;
        } else if(quote) { if(c==quote) quote=0; else word+=c; token=true; }
        else if(c=='\'' || c=='"') {quote=c;token=true;}
        else if(c==' '||c=='\n'||c=='\r'||c=='\t') { if(token) {result.push_back(word);word.clear();token=false;} }
        else {word+=c;token=true;}
    }
    iro::require(!quote,"unfinished shell quote"); if(token) result.push_back(word); return result;
}
inline void checked(const std::vector<std::string> &cmd,const fs::path &cwd={}) {
    int rc=dt::run(cmd,cwd); if(rc) throw std::runtime_error("command failed ("+std::to_string(rc)+"): "+cmd[0]);
}
inline void append(std::vector<std::string> &to,const std::vector<std::string> &from) { to.insert(to.end(),from.begin(),from.end()); }
inline void output(std::vector<std::string> &cmd,const fs::path &path) {
    auto i=std::find(cmd.begin(),cmd.end(),"-o"); iro::require(i!=cmd.end()&&i+1!=cmd.end(),"link command lacks -o"); *(i+1)=path.string();
}
inline std::vector<std::pair<std::string,std::string>> gte_declarations(const fs::path &root) {
    std::string header=dt::read(root/"runtime/gte.h"); std::regex re("void (gte_\\w+)\\(([^;]+)\\);");
    std::vector<std::pair<std::string,std::string>> result;
    for(auto i=std::sregex_iterator(header.begin(),header.end(),re);i!=std::sregex_iterator();++i) result.emplace_back((*i)[1],(*i)[2]);
    return result;
}
inline fs::path frame_probe(const fs::path &root,fs::path build,fs::path dest,bool subsystems) {
    build=fs::weakly_canonical(build);dest=fs::weakly_canonical(dest);fs::create_directories(dest.parent_path());
    std::string source=dt::read(root/"tools/frame_probe.c");
    std::vector<std::string> options;
    if(dt::read(build/"CMakeCache.txt").find("FF8_STATS:BOOL=ON")!=std::string::npos) options.push_back("-DFF8_STATS");
    std::vector<std::string> symbols={"cpu_init","host_present","hw_destroy"};
    if(subsystems) {
        options.push_back("-DSUBSYSTEMS");
        append(symbols,{"gte_cmd","gte_read","gte_write","gpu_gp0","gpu_gp0_words","gpu_gp1","gpu_read","cdrom_event","dma_event","dma_sync","spu_advance","spu_read","spu_write","spu_dma_write","spu_dma_read"});
        for(const auto &p:gte_declarations(root)) {
            std::istringstream params(p.second);std::string param,decl,args;int index=0;
            while(std::getline(params,param,',')) {
                param=std::regex_replace(param,std::regex("\\b\\w+$"),"");
                param=std::regex_replace(param,std::regex("^\\s+|\\s+$"),"");
                std::string name="a"+std::to_string(index++);
                if(!decl.empty()) {decl+=',';args+=',';} decl+=param+" "+name;args+=name;
            }
            source+="\nWRAP_VOID("+p.first+",1,("+decl+"),("+args+"))\n";symbols.push_back(p.first);
        }
    }
    auto cmd=split(dt::read(build/"CMakeFiles/ff8.dir/link.txt"));output(cmd,dest);
    std::string pattern=(dest.parent_path()/"frame-probe-XXXXXX").string();std::vector<char> buffer(pattern.begin(),pattern.end());buffer.push_back(0);
    char *created=mkdtemp(buffer.data());iro::require(created,"cannot create probe directory");fs::path temp=created;
    try {
        auto src=temp/"probe.c",obj=temp/"probe.o";dt::write(src,source);
        const char *cc=getenv("CC");std::vector<std::string> compile={cc?cc:"cc","-O2","-std=c99","-I"+(root/"runtime").string()};append(compile,options);append(compile,{"-c",src.string(),"-o",obj.string()});checked(compile);
        std::vector<std::string> wrap{obj.string()};for(const auto &s:symbols) wrap.push_back("-Wl,--wrap="+s);
        cmd.insert(cmd.begin()+1,wrap.begin(),wrap.end());checked(cmd,build);
    } catch(...) {fs::remove_all(temp);throw;}
    fs::remove_all(temp);return dest;
}
}
