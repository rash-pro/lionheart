#include "perf_report.h"
#include <iostream>
int main(int argc,char **argv) {
    try {
        std::map<std::string,std::string> args;
        for(int i=1;i<argc;++i) {
            std::string key=argv[i];
            if(key=="--help") { std::cout<<"Usage: perf-report --log FILE --first N --last N --output FILE [--histogram CSV]\n"; return 0; }
            if(i+1==argc || (key!="--log"&&key!="--first"&&key!="--last"&&key!="--output"&&key!="--histogram")) throw std::runtime_error("unexpected argument: "+key);
            args[key]=argv[++i];
        }
        int first=std::stoi(args.at("--first")),last=std::stoi(args.at("--last"));
        iro::require(first<last,"first must precede last");
        auto result=perf::summarize(dt::read(args.at("--log")),first,last,args["--histogram"]);
        dt::write(args.at("--output"),result.dump(2)+"\n");
        auto summary=dt::Json::object();
        for(const auto &p:std::get<dt::Json::Object>(result.value)) if(p.first!="candidates"&&p.first!="hot_pcs") summary.set(p.first,p.second);
        std::cout<<summary.dump(2)<<'\n';
    } catch(const std::exception &e) { std::cerr<<"perf-report: "<<e.what()<<'\n'; return 1; }
}
