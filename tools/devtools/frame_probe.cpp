#include "frame_probe.h"
#include <iostream>
int main(int argc,char **argv) {
    try {
        std::filesystem::path build,output;bool subsystems=false;
        for(int i=1;i<argc;++i) {
            std::string arg=argv[i];
            if(arg=="--output"&&i+1<argc) output=argv[++i];
            else if(arg=="--subsystems") subsystems=true;
            else if(arg=="--help") {std::cout<<"Usage: frame-probe BUILD --output FILE [--subsystems]\n";return 0;}
            else if(build.empty()&&arg[0]!='-') build=arg;else throw std::runtime_error("unexpected argument: "+arg);
        }
        iro::require(!build.empty()&&!output.empty(),"BUILD and --output are required");
        std::cout<<trace::frame_probe(LH_TOOLS_ROOT,build,output,subsystems).string()<<'\n';
    } catch(const std::exception &e) {std::cerr<<"frame-probe: "<<e.what()<<'\n';return 1;}
}
