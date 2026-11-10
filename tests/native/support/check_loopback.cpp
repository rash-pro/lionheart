// Linux syscall audit for these host tests, independent of either HTTP stack.
// Follow every process and worker thread, including the separately built GUI.
#include <arpa/inet.h>
#include <cerrno>
#include <csignal>
#include <cstring>
#include <iostream>
#include <set>
#include <stdexcept>
#include <sys/ptrace.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/user.h>
#include <sys/wait.h>
#include <unistd.h>

#if !defined(__linux__) || !defined(__x86_64__)
#error This disposable syscall audit requires a Linux x86_64 host
#endif
namespace {
void read_memory(pid_t pid, unsigned long address, void *out, size_t size) {
    for (size_t at = 0; at < size; at += sizeof(long)) {
        errno = 0;
        long word = ptrace(PTRACE_PEEKDATA, pid, address + at, nullptr);
        if (word == -1 && errno)
            throw std::runtime_error("cannot read syscall address");
        memcpy(static_cast<char *>(out) + at, &word, std::min(sizeof(word), size - at));
    }
}
void check_address(pid_t pid, const char *operation, unsigned long address, size_t length, size_t &count) {
    if (!address || !length)
        return;
    sockaddr_storage data{};
    read_memory(pid, address, &data, std::min(length, sizeof(data)));
    char text[INET6_ADDRSTRLEN]{};
    unsigned port = 0;
    bool local = false;
    if (data.ss_family == AF_INET) {
        const auto &a = reinterpret_cast<const sockaddr_in &>(data);
        inet_ntop(AF_INET, &a.sin_addr, text, sizeof(text));
        port = ntohs(a.sin_port);
        local = (ntohl(a.sin_addr.s_addr) >> 24) == 127;
    } else if (data.ss_family == AF_INET6) {
        const auto &a = reinterpret_cast<const sockaddr_in6 &>(data);
        inet_ntop(AF_INET6, &a.sin6_addr, text, sizeof(text));
        port = ntohs(a.sin6_port);
        local = IN6_IS_ADDR_LOOPBACK(&a.sin6_addr);
    } else if (data.ss_family == AF_UNIX || data.ss_family == AF_NETLINK || data.ss_family == AF_UNSPEC) {
        return;
    } else {
        throw std::runtime_error("unexpected socket address family " + std::to_string(data.ss_family));
    }
    std::cout << pid << ' ' << operation << ' ' << text << ':' << port << '\n';
    if (!local)
        throw std::runtime_error(std::string("non-loopback socket: ") + operation + " " + text);
    ++count;
}
} // namespace
int main(int argc, char **argv) {
    if (argc < 2)
        return 2;
    pid_t child = fork();
    if (child < 0)
        return 2;
    if (!child) {
        setpgid(0, 0);
        if (ptrace(PTRACE_TRACEME, 0, nullptr, nullptr) < 0)
            _exit(126);
        raise(SIGSTOP);
        execvp(argv[1], argv + 1);
        _exit(127);
    }
    std::set<pid_t> live{child};
    int root_rc = 0;
    size_t count = 0, traced = 1;
    try {
        int status;
        if (waitpid(child, &status, 0) != child || !WIFSTOPPED(status))
            throw std::runtime_error("cannot start syscall audit");
        long options = PTRACE_O_TRACESYSGOOD | PTRACE_O_TRACEFORK | PTRACE_O_TRACEVFORK |
                       PTRACE_O_TRACECLONE | PTRACE_O_TRACEEXEC | PTRACE_O_EXITKILL;
        if (ptrace(PTRACE_SETOPTIONS, child, nullptr, options) < 0)
            throw std::runtime_error("cannot set syscall audit options");
        ptrace(PTRACE_SYSCALL, child, nullptr, nullptr);
        while (!live.empty()) {
            pid_t pid = waitpid(-1, &status, __WALL);
            if (pid < 0) {
                if (errno == EINTR)
                    continue;
                throw std::runtime_error("wait syscall audit");
            }
            if (WIFEXITED(status) || WIFSIGNALED(status)) {
                if (pid == child)
                    root_rc = WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
                live.erase(pid);
                continue;
            }
            unsigned event = unsigned(status) >> 16;
            if (event == PTRACE_EVENT_FORK || event == PTRACE_EVENT_VFORK || event == PTRACE_EVENT_CLONE) {
                unsigned long next = 0;
                ptrace(PTRACE_GETEVENTMSG, pid, nullptr, &next);
                live.insert(pid_t(next));
                ++traced;
            }
            int signal = WSTOPSIG(status);
            if (signal == (SIGTRAP | 0x80)) {
                user_regs_struct r{};
                if (ptrace(PTRACE_GETREGS, pid, nullptr, &r) < 0)
                    throw std::runtime_error("read syscall registers");
                // Linux sets rax to -ENOSYS at syscall entry on this architecture.
                if (static_cast<long long>(r.rax) == -ENOSYS) {
                    if (r.orig_rax == SYS_connect || r.orig_rax == SYS_bind)
                        check_address(pid, r.orig_rax == SYS_connect ? "connect" : "bind", r.rsi, r.rdx,
                                      count);
                    else if (r.orig_rax == SYS_sendto)
                        check_address(pid, "sendto", r.r8, r.r9, count);
                    else if (r.orig_rax == SYS_sendmsg || r.orig_rax == SYS_sendmmsg) {
                        size_t n = r.orig_rax == SYS_sendmsg ? 1 : r.rdx;
                        for (size_t i = 0; i < n; ++i) {
                            msghdr m{};
                            read_memory(pid, r.rsi + i * sizeof(mmsghdr), &m, sizeof(m));
                            check_address(pid, "sendmsg", reinterpret_cast<unsigned long>(m.msg_name),
                                          m.msg_namelen, count);
                        }
                    }
                }
                signal = 0;
            } else if (signal == SIGTRAP || signal == SIGSTOP)
                signal = 0;
            if (ptrace(PTRACE_SYSCALL, pid, nullptr, signal) < 0 && errno != ESRCH)
                throw std::runtime_error("resume syscall audit");
        }
        std::cout << "LOOPBACK AUDIT: " << count << " IPv4/IPv6 address operations, " << traced
                  << " processes/threads, no external address, child rc=" << root_rc << '\n';
        return root_rc;
    } catch (const std::exception &e) {
        kill(-child, SIGKILL);
        for (auto pid : live)
            kill(pid, SIGKILL);
        std::cerr << "FAIL loopback audit: " << e.what() << '\n';
        return 1;
    }
}
