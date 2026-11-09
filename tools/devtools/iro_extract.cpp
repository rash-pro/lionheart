#include "iro_extract.h"
#include <cstdlib>
#include <iostream>
int main(int argc,char **argv) {
    try {
        std::filesystem::path archive,output; bool list=false;
        const char *cache=std::getenv("XDG_CACHE_HOME"),*home=std::getenv("HOME");
        output=(cache&&*cache?std::filesystem::path(cache):std::filesystem::path(home?home:".")/".cache")/"ff8-recomp/movies/cinema-rf";
        for(int i=1;i<argc;++i) {
            std::string arg=argv[i];
            if(arg=="--list") list=true;
            else if(arg=="--output" && i+1<argc) output=argv[++i];
            else if(arg=="--help") { std::cout << "Usage: iro-extract archive [--list] [--output DIRECTORY]\n"; return 0; }
            else if(archive.empty() && arg[0]!='-') archive=arg;
            else throw std::runtime_error("unexpected argument: "+arg);
        }
        if(archive.empty()) throw std::runtime_error("archive is required");
        auto entries=iro::directory(archive);
        if(list) std::cout << iro::listing(entries);
        else std::cout << "Extracted " << iro::extract(archive,output,entries,LH_TOOLS_ROOT) << " movies/cameras to " << output.string() << '\n';
    } catch(const std::exception &e) { std::cerr << "iro-extract: " << e.what() << '\n'; return 1; }
}
