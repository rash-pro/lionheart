#pragma once
#include <algorithm>
#include <codecvt>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <locale>
#include <set>
#include <sstream>
#include <stdexcept>
#include <vector>
#include <fcntl.h>
#include <unistd.h>
namespace iro {
namespace fs=std::filesystem;
struct Entry { std::string name; uint32_t flags; uint64_t offset; uint32_t size; };
inline void require(bool ok,const char *why) { if(!ok) throw std::runtime_error(why); }
inline uint64_t integer(std::istream &in,int n) {
    uint64_t value=0;
    for(int i=0;i<n;++i) { int b=in.get(); require(b!=EOF,"truncated IROS directory"); value |= uint64_t(b)<<(8*i); }
    return value;
}
inline std::vector<Entry> directory(const fs::path &path) {
    auto size=fs::file_size(path); std::ifstream in(path,std::ios::binary);
    require(size>=16,"truncated IROS header");
    char magic[4]; in.read(magic,4);
    auto version=integer(in,4),flags=integer(in,4),at=integer(in,4);
    require(std::string(magic,4)=="IROS" && version==0x10002 && !flags && at>=16 && at<=size-4,"unsupported IROS header");
    in.seekg(at); auto count=integer(in,4); require(count<=100000,"invalid IROS entry count");
    std::vector<Entry> entries;
    for(uint64_t i=0;i<count;++i) {
        auto start=uint64_t(in.tellg()); auto length=integer(in,2),name_length=integer(in,2);
        require(name_length%2==0 && length>=name_length+20 && start+length<=size,"invalid IROS directory record");
        std::u16string wide;
        for(uint64_t j=0;j<name_length;j+=2) wide.push_back(char16_t(integer(in,2)));
        std::string name=std::wstring_convert<std::codecvt_utf8_utf16<char16_t>,char16_t>{}.to_bytes(wide);
        std::replace(name.begin(),name.end(),'\\','/');
        bool safe=!name.empty() && name[0]!='/' && name.find(':')==std::string::npos;
        for(const auto &p:fs::path(name)) if(p=="..") safe=false;
        require(safe,"unsafe IROS entry name");
        Entry e{name,uint32_t(integer(in,4)),integer(in,8),uint32_t(integer(in,4))};
        require(e.offset<=size && e.size<=size-e.offset,"IROS entry extends beyond archive");
        entries.push_back(e); in.seekg(start+length);
    }
    auto end=uint64_t(in.tellg());
    for(const auto &e:entries) require(e.offset>=end,"IROS data overlaps directory");
    return entries;
}
inline std::string lower(std::string s) { for(char &c:s) if(c>='A'&&c<='Z') c+=32; return s; }
inline bool inside(const fs::path &path,const fs::path &root) {
    auto relative=path.lexically_relative(root);
    return !relative.empty() && *relative.begin()!="..";
}
inline fs::path relative(const Entry &e) {
    fs::path result; bool language=false;
    for(const auto &part:fs::path(e.name)) { if(part=="lang-en") language=true; if(language) result/=part; }
    return language?result:fs::path(e.name).filename();
}
inline size_t extract(const fs::path &path,fs::path destination,const std::vector<Entry> &entries,const fs::path &root) {
    destination=fs::weakly_canonical(destination);
    require(!inside(destination,fs::canonical(root)),"extract user assets outside the repository");
    std::vector<Entry> selected; std::set<std::string> names;
    for(const auto &e:entries) {
        auto ext=lower(fs::path(e.name).extension().string());
        if(ext!=".avi" && ext!=".cam") continue;
        require(!e.flags,"compressed IROS entries are unsupported; no files extracted");
        selected.push_back(e);
    }
    for(const auto &e:selected) require(names.insert(lower(relative(e).string())).second,"duplicate movie basenames");
    fs::create_directories(destination); std::ifstream in(path,std::ios::binary);
    for(const auto &e:selected) {
        auto target=destination/relative(e);
        require(inside(fs::weakly_canonical(target),destination),"destination escapes cache through a symlink");
        fs::create_directories(target.parent_path()); require(!fs::is_symlink(target),"refusing a symlink destination");
        in.clear(); in.seekg(e.offset);
        if(fs::is_regular_file(target) && fs::file_size(target)==e.size) {
            std::ifstream cached(target,std::ios::binary); uint64_t remaining=e.size; bool equal=true;
            char a[65536],b[65536];
            while(remaining) { auto n=std::min<uint64_t>(remaining,sizeof(a)); in.read(a,n); cached.read(b,n); if(!std::equal(a,a+n,b)) { equal=false; break; } remaining-=n; }
            if(equal) continue;
        }
        auto temporary=target; temporary += ".part";
        int fd=::open(temporary.c_str(),O_WRONLY|O_CREAT|O_EXCL,0666);
        if(fd<0) throw std::runtime_error("remove incomplete extraction first: "+temporary.string());
        try {
            in.clear(); in.seekg(e.offset); uint64_t remaining=e.size; char buffer[65536];
            while(remaining) {
                auto n=std::min<uint64_t>(remaining,sizeof(buffer)); in.read(buffer,n); require(uint64_t(in.gcount())==n,"truncated IROS data");
                size_t done=0; while(done<n) { auto wrote=::write(fd,buffer+done,n-done); require(wrote>0,"cannot write IROS data"); done+=wrote; }
                remaining-=n;
            }
            require(::close(fd)==0,"cannot close IROS data"); fd=-1; fs::rename(temporary,target);
        } catch(...) { if(fd>=0) ::close(fd); fs::remove(temporary); throw; }
    }
    return selected.size();
}
inline std::string quoted(const std::string &s) {
    std::ostringstream out; out << '"';
    for(unsigned char c:s) {
        if(c=='"'||c=='\\') out << '\\' << c;
        else if(c<32 || c>=127) {
            // UTF-8 is decoded below so JSON uses the same ASCII escapes as the inspector.
            std::u16string wide=std::wstring_convert<std::codecvt_utf8_utf16<char16_t>,char16_t>{}.from_bytes(s);
            out.str(""); out.clear(); out << '"';
            for(char16_t w:wide) {
                if(w=='"'||w=='\\') out << '\\' << char(w);
                else if(w=='\n') out << "\\n"; else if(w=='\r') out << "\\r"; else if(w=='\t') out << "\\t";
                else if(w=='\b') out << "\\b"; else if(w=='\f') out << "\\f";
                else if(w<32||w>=127) out << "\\u" << std::hex << std::setw(4) << std::setfill('0') << unsigned(w);
                else out << char(w);
            }
            return out.str()+'"';
        } else out << c;
    }
    return out.str()+'"';
}
inline std::string listing(const std::vector<Entry> &entries) {
    if(entries.empty()) return "[]\n";
    std::ostringstream out; out << "[\n";
    for(size_t i=0;i<entries.size();++i) {
        const auto &e=entries[i];
        out << "  {\n    \"name\": " << quoted(e.name) << ",\n    \"flags\": " << e.flags << ",\n    \"offset\": " << e.offset << ",\n    \"size\": " << e.size << "\n  }" << (i+1<entries.size()?",":"") << '\n';
    }
    return out.str()+"]\n";
}
}
