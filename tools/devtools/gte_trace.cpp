#include "gte_trace.h"
#include <iostream>
#include "trace_cli.h"
int main(int argc,char **argv) {
    trace::Arguments arguments(argc,argv);argc=int(arguments.words.size());argv=arguments.pointers.data();
    try {
        std::string mode,cc=getenv("CC")?getenv("CC"):"cc";trace::fs::path host,dest=trace::fs::path(LH_TOOLS_ROOT)/"build/gte";
        for(int i=1;i<argc;++i) {
            std::string a=argv[i];
            if(a=="--help") {std::cout<<"Usage: gte-trace {record,replay} [--host-build DIR] [--output DIR] [--compiler CC]\n";return 0;}
            if(a=="record"||a=="replay") mode=a;
            else {iro::require(i+1<argc,"missing option value");std::string v=argv[++i];if(a=="--host-build")host=v;else if(a=="--output")dest=v;else if(a=="--compiler")cc=v;else throw std::runtime_error("unexpected option: "+a);}
        }
        iro::require(mode=="record"||mode=="replay","record or replay is required");
        iro::require(mode!="record"||!host.empty(),"record requires --host-build");
        auto binary=mode=="replay"?trace::gte_replay(LH_TOOLS_ROOT,dest,cc):trace::gte_record(LH_TOOLS_ROOT,host,dest,cc);std::cout<<binary.string()<<'\n';
    } catch(const std::exception &e) {std::cerr<<"gte-trace: "<<e.what()<<'\n';return 1;}
}
