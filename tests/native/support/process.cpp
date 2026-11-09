// Starts a program with a deadline and captures both output streams.
#include "lh_internal.h"

#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <sys/wait.h>
#include <unistd.h>

namespace lh::internal {

// Wine reads paths in its own namespace; everything outside it needs a drive letter.
std::string to_windows_path(const std::string &path) {
    if (path.empty() || path[0] != '/') return path;
    std::string out = "Z:" + path;
    for (char &c : out) if (c == '/') c = '\\';
    return out;
}

namespace {

std::vector<std::string> argv_for(const Command &command) {
    const BuildInfo &build = build_info();
    std::vector<std::string> argv;
    if (command.target()) argv.insert(argv.end(), build.emulator.begin(), build.emulator.end());
    argv.push_back(command.program());
    for (const Arg &arg : command.arguments()) {
        argv.push_back(arg.is_path() && command.target() && build.windows_paths ? to_windows_path(arg.text()) : arg.text());
    }
    return argv;
}

std::string quoted(const std::vector<std::string> &argv) {
    std::string out;
    for (const std::string &arg : argv) {
        bool plain = !arg.empty() && arg.find_first_not_of(
            "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_./:=,+-") == std::string::npos;
        if (!out.empty()) out += ' ';
        if (plain) { out += arg; continue; }
        out += '\'';
        for (char c : arg) { if (c == '\'') out += "'\\''"; else out += c; }
        out += '\'';
    }
    return out;
}

[[noreturn]] void child(const Command &command, const std::vector<std::string> &argv,
                        int out, int err, int status, int in) {
    // A group of its own lets a timeout also stop anything the program started.
    setpgid(0, 0);
    if (in >= 0) {
        dup2(in, 0);
    } else {
        // A missing input file is the test's mistake; report it instead of running on an empty stdin.
        int source = open(command.input_file().empty() ? "/dev/null" : command.input_file().c_str(), O_RDONLY);
        if (source < 0) {
            int code = errno;
            (void)!write(status, &code, sizeof(code));
            _exit(127);
        }
        dup2(source, 0);
    }
    dup2(out, 1);
    dup2(err, 2);
    if (!command.working_directory().empty() && chdir(command.working_directory().c_str()) != 0) {
        int code = errno;
        (void)!write(status, &code, sizeof(code));
        _exit(127);
    }
    for (const std::string &name : command.unset_names()) unsetenv(name.c_str());
    for (const auto &entry : command.environment()) setenv(entry.first.c_str(), entry.second.c_str(), 1);
    std::vector<char *> raw;
    for (const std::string &arg : argv) raw.push_back(const_cast<char *>(arg.c_str()));
    raw.push_back(nullptr);
    execvp(raw[0], raw.data());
    int code = errno;
    (void)!write(status, &code, sizeof(code));
    _exit(127);
}

} // namespace

Result run_process(const Command &command) {
    Result result;
    std::vector<std::string> argv = argv_for(command);
    result.command = quoted(argv);
    result.timeout = command.timeout();
    int out[2], err[2], status[2], in[2] = {-1, -1};
    if (pipe2(out, O_CLOEXEC) != 0 || pipe2(err, O_CLOEXEC) != 0 || pipe2(status, O_CLOEXEC) != 0 ||
        (command.has_input() && pipe2(in, O_CLOEXEC) != 0)) {
        LH_FAIL(std::string("cannot create pipes: ") + std::strerror(errno));
    }
    // A program that exits before reading all its input must not kill the runner.
    if (command.has_input()) signal(SIGPIPE, SIG_IGN);
    fflush(stdout);
    fflush(stderr);
    pid_t pid = fork();
    if (pid < 0) LH_FAIL(std::string("cannot fork: ") + std::strerror(errno));
    if (pid == 0) child(command, argv, out[1], err[1], status[1], in[0]);
    close(out[1]);
    close(err[1]);
    close(status[1]);
    if (in[0] >= 0) close(in[0]);
    int exec_errno = 0;
    if (read(status[0], &exec_errno, sizeof(exec_errno)) == (ssize_t)sizeof(exec_errno)) {
        close(status[0]);
        close(out[0]);
        close(err[0]);
        if (in[1] >= 0) close(in[1]);
        waitpid(pid, nullptr, 0);
        LH_FAIL("cannot start " + result.command + ": " + std::strerror(exec_errno));
    }
    close(status[0]);

    auto deadline = std::chrono::steady_clock::now() + std::chrono::duration<double>(command.timeout());
    // Input is written from this same loop: a program may fill its output pipe
    // before it has read everything, and a blocking write would then hang both sides.
    if (in[1] >= 0) {
        if (command.input_bytes().empty()) {
            close(in[1]);
            in[1] = -1;
        } else {
            fcntl(in[1], F_SETFL, O_NONBLOCK);
        }
    }
    size_t written = 0;
    struct pollfd fds[3] = {{out[0], POLLIN, 0}, {err[0], POLLIN, 0}, {in[1], POLLOUT, 0}};
    int wait_status = 0;
    bool reaped = false;
    char buffer[65536];
    for (;;) {
        bool open_stream = fds[0].fd >= 0 || fds[1].fd >= 0 || (!reaped && fds[2].fd >= 0);
        // After the program exits, keep draining only what is already buffered:
        // a helper it left running may hold the pipes open forever.
        int waited = reaped ? 0 : 50;
        if (open_stream) {
            if (reaped && fds[2].fd >= 0) {
                close(fds[2].fd);
                fds[2].fd = -1;
            }
            int ready = poll(fds, 3, waited);
            if (ready < 0 && errno != EINTR) LH_FAIL(std::string("poll failed: ") + std::strerror(errno));
            if (ready > 0 && fds[2].fd >= 0 && (fds[2].revents & (POLLOUT | POLLERR | POLLHUP))) {
                const std::string &bytes = command.input_bytes();
                ssize_t n = write(fds[2].fd, bytes.data() + written, bytes.size() - written);
                if (n > 0) written += static_cast<size_t>(n);
                else if (n < 0 && errno != EINTR && errno != EAGAIN) written = bytes.size();
                if (written == bytes.size()) {
                    close(fds[2].fd);
                    fds[2].fd = -1;
                }
            }
            for (int i = 0; ready > 0 && i < 2; ++i) {
                if (fds[i].fd < 0 || !(fds[i].revents & (POLLIN | POLLHUP | POLLERR))) continue;
                ssize_t n = read(fds[i].fd, buffer, sizeof(buffer));
                if (n > 0) (i == 0 ? result.out : result.err).append(buffer, static_cast<size_t>(n));
                else if (n == 0 || (n < 0 && errno != EINTR && errno != EAGAIN)) {
                    close(fds[i].fd);
                    fds[i].fd = -1;
                }
            }
            if (ready == 0 && reaped) break;
        } else if (reaped) {
            break;
        } else {
            usleep(1000);
        }
        if (!reaped && waitpid(pid, &wait_status, WNOHANG) == pid) reaped = true;
        if (!reaped && std::chrono::steady_clock::now() >= deadline) {
            result.timed_out = true;
            killpg(pid, SIGKILL);
            kill(pid, SIGKILL);
            waitpid(pid, &wait_status, 0);
            reaped = true;
        }
    }
    for (struct pollfd &fd : fds) if (fd.fd >= 0) close(fd.fd);
    if (WIFEXITED(wait_status)) result.rc = WEXITSTATUS(wait_status);
    else if (WIFSIGNALED(wait_status)) result.rc = -WTERMSIG(wait_status);
    return result;
}

} // namespace lh::internal
