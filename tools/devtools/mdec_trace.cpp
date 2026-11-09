#include "mdec_trace.h"
#include <iostream>
#include "trace_cli.h"
#include <cstring>
int main(int argc,char **argv) {
    trace::Arguments arguments(argc,argv);argc=int(arguments.words.size());argv=arguments.pointers.data();
    try {
        namespace fs=std::filesystem;
        if(argc==2&&std::string(argv[1])=="--help"){std::cout<<"Usage: mdec-trace build --host-build DIR --output DIR [--runtime DIR] | {record,replay} --trace FILE -- COMMAND...\n";return 0;}
        iro::require(argc>1,"build, record or replay is required");std::string mode=argv[1];
        fs::path root=LH_TOOLS_ROOT,host,dest,runtime=root/"runtime",file;std::vector<std::string> command;
        for(int i=2;i<argc;++i) {
            std::string a=argv[i];if(a=="--") {while(++i<argc) command.push_back(argv[i]);break;}
            if(a.rfind("--",0)!=0) {do {command.push_back(argv[i]);}while(++i<argc);break;}
            iro::require(i+1<argc,"missing option value");std::string v=argv[++i];
            if(a=="--host-build")host=v;else if(a=="--output")dest=v;else if(a=="--runtime")runtime=v;else if(a=="--trace")file=v;else throw std::runtime_error("unexpected option: "+a);
        }
        if(mode=="build") {
            iro::require(!host.empty()&&!dest.empty(),"--host-build and --output are required");host=fs::weakly_canonical(host);dest=fs::weakly_canonical(dest);runtime=fs::weakly_canonical(runtime);
            iro::require(fs::create_directories(dest),"output already exists");
            auto text=dt::read(host/"CMakeFiles/ff8_runtime.dir/flags.make");std::vector<std::string> options;
            for(const char *key:{"C_DEFINES","C_INCLUDES","C_FLAGS"}) trace::append(options,trace::flags(text,key));
            options.erase(std::remove_if(options.begin(),options.end(),[](const auto &s){return s.rfind("-I",0)==0;}),options.end());options.push_back("-I"+runtime.string());
            auto source=dt::read(runtime/"hw.c");
            source=trace::instrument_hw(source);
            dt::write(dest/"hw.c",source);
            for(const char *name:{"hw","trace"}) {std::vector<std::string> cmd={"cc"};trace::append(cmd,options);trace::append(cmd,{"-c",(std::string(name)=="hw"?dest/"hw.c":root/"tools/mdec_trace.c").string(),"-o",(dest/(std::string(name)+".o")).string()});trace::checked(cmd);}
            command=trace::split(dt::read(host/"CMakeFiles/ff8.dir/link.txt"));trace::output(command,dest/"ff8");
            auto archive=std::find_if(command.begin(),command.end(),[](const auto &s){return s.size()>2&&s.substr(s.size()-2)==".a";});iro::require(archive!=command.end(),"link command lacks archive");command.insert(archive,{(dest/"hw.o").string(),(dest/"trace.o").string()});
            command.erase(std::remove_if(command.begin(),command.end(),[](const auto &s){return s.rfind("-Wl,--dependency-file=",0)==0;}),command.end());trace::checked(command,host);std::cout<<(dest/"ff8").string()<<'\n';return 0;
        }
        iro::require(mode=="record"||mode=="replay","unknown mode");iro::require(!file.empty(),"--trace is required");file=fs::weakly_canonical(file);
        auto rel=file.lexically_relative(root/"build");iro::require(!rel.empty()&&*rel.begin()!="..","disc-derived traces must stay under build/");
        iro::require(!command.empty(),"supply the instrumented host command after --");iro::require(mode!="record"||!fs::exists(file),"use a new trace file");
        unsetenv("FF8_MDEC_TRACE");unsetenv("FF8_MDEC_REPLAY");return dt::run(command,root,{},{{mode=="record"?"FF8_MDEC_TRACE":"FF8_MDEC_REPLAY",file.string()}});
    } catch(const std::exception &e) {std::cerr<<"mdec-trace: "<<e.what()<<'\n';return 1;}
}
