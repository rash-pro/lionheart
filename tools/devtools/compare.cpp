#include "compare.h"
#include <iostream>
int main(int argc,char **argv) {
    try {
        std::map<std::string,std::string> args{{"--script","tests/scripts/new-game.txt"},{"--vblanks","15000"},{"--output","build/exactness"}};
        std::vector<std::filesystem::path> discs; std::vector<std::string> before_args,after_args;
        for(int i=1;i<argc;++i) {
            std::string key=argv[i];
            if(key=="--help") {std::cout<<"Usage: compare --before FILE --after FILE --exe FILE --disc FILE [--script FILE] [--card FILE] [--vblanks N] [--output DIR] [--before-arg ARG] [--after-arg ARG]\n";return 0;}
            auto eq=key.find('='); std::string value;
            if(eq!=std::string::npos) {value=key.substr(eq+1);key.resize(eq);} else {iro::require(i+1<argc,"missing option value");value=argv[++i];}
            if(key=="--disc") discs.emplace_back(value);
            else if(key=="--before-arg") before_args.push_back(value);
            else if(key=="--after-arg") after_args.push_back(value);
            else if(key=="--before"||key=="--after"||key=="--exe"||key=="--script"||key=="--vblanks"||key=="--card"||key=="--output") args[key]=value;
            else throw std::runtime_error("unexpected argument: "+key);
        }
        iro::require(!discs.empty(),"--disc is required");
        auto before=comparison::run(args.at("--before"),args.at("--exe"),discs,args.at("--script"),std::stoi(args.at("--vblanks")),std::filesystem::path(args.at("--output"))/"before",args["--card"],{},before_args);
        auto after=comparison::run(args.at("--after"),args.at("--exe"),discs,args.at("--script"),std::stoi(args.at("--vblanks")),std::filesystem::path(args.at("--output"))/"after",args["--card"],{},after_args);
        auto result=comparison::compare(before,after); dt::write(std::filesystem::path(args.at("--output"))/"comparison.json",result.dump(2)+"\n"); std::cout<<result.dump()<<'\n';
    } catch(const std::exception &e) {std::cerr<<"compare: "<<e.what()<<'\n';return 1;}
}
