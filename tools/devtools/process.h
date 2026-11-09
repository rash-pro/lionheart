#pragma once
#include "json.h"
#include <map>
#include <cerrno>
#include <sys/wait.h>
namespace dt {
inline int run(const std::vector<std::string> &command,const std::filesystem::path &cwd={},const std::filesystem::path &log={},const std::map<std::string,std::string> &env={}) {
    iro::require(!command.empty(),"empty command");
    pid_t pid=fork(); iro::require(pid>=0,"cannot fork");
    if(!pid) {
        if(!cwd.empty() && chdir(cwd.c_str())) _exit(126);
        if(!log.empty()) { int fd=open(log.c_str(),O_WRONLY|O_CREAT|O_TRUNC,0666); if(fd<0) _exit(126); dup2(fd,1); dup2(fd,2); close(fd); }
        for(const auto &p:env) setenv(p.first.c_str(),p.second.c_str(),1);
        std::vector<char *> args; for(const auto &s:command) args.push_back(const_cast<char *>(s.c_str())); args.push_back(nullptr);
        execvp(args[0],args.data()); _exit(127);
    }
    int status; while(waitpid(pid,&status,0)<0) if(errno!=EINTR) throw std::runtime_error("cannot wait for child");
    return WIFEXITED(status)?WEXITSTATUS(status):-WTERMSIG(status);
}
}
