#pragma once
#include <string>
#include <vector>
namespace trace {
// Keep option values literal while accepting argparse's --option=value spelling.
struct Arguments {
    std::vector<std::string> words;
    std::vector<char *> pointers;
    Arguments(int argc,char **argv) {
        bool remainder=false;
        for(int i=0;i<argc;++i) {
            std::string s=argv[i];auto pos=s.find('=');
            if(!remainder&&s.rfind("--",0)==0&&pos!=std::string::npos) {words.push_back(s.substr(0,pos));words.push_back(s.substr(pos+1));}
            else words.push_back(s);
            if(s=="--") remainder=true;
        }
        for(auto &s:words)pointers.push_back(s.data());pointers.push_back(nullptr);
    }
};
}
