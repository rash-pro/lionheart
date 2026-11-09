// Socket peers stay local and use deadlines so a broken protocol fails the
// case.
#pragma once
#include "lh_test.h"
#include <arpa/inet.h>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>

namespace lh {
inline void socket_check(bool ok, const char *what) {
    if (!ok)
        throw Failure(std::string(what) + ": " + std::strerror(errno));
}
class Socket {
  public:
    explicit Socket(int fd = -1) : fd_(fd) {}
    ~Socket() {
        if (fd_ >= 0)
            ::close(fd_);
    }
    Socket(const Socket &) = delete;
    Socket &operator=(const Socket &) = delete;
    Socket(Socket &&other) noexcept : fd_(other.release()) {}
    Socket &operator=(Socket &&other) noexcept {
        if (fd_ >= 0)
            ::close(fd_);
        fd_ = other.release();
        return *this;
    }
    int get() const { return fd_; }
    int release() {
        int out = fd_;
        fd_ = -1;
        return out;
    }
    void nonblocking() const {
        socket_check(fcntl(fd_, F_SETFL, fcntl(fd_, F_GETFL) | O_NONBLOCK) == 0, "nonblocking socket");
    }
    void send(const std::string &bytes) const {
        size_t at = 0;
        while (at < bytes.size()) {
            const auto n = ::send(fd_, bytes.data() + at, bytes.size() - at, MSG_NOSIGNAL);
            if (n < 0 && errno == EINTR)
                continue;
            socket_check(n > 0, "send socket");
            at += size_t(n);
        }
    }
    // Like the pytest helper: drain available bytes, stopping at an SSE boundary.
    std::string receive(int timeout_ms = 0) const {
        std::string out;
        pollfd p{fd_, POLLIN, 0};
        socket_check(poll(&p, 1, timeout_ms) >= 0, "poll socket");
        for (;;) {
            char bytes[65536];
            auto n = recv(fd_, bytes, sizeof(bytes), MSG_DONTWAIT);
            if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
                break;
            if (n < 0 && errno == EINTR)
                continue;
            socket_check(n >= 0, "receive socket");
            if (!n)
                break;
            out.append(bytes, size_t(n));
            if (out.find("\n\n") != std::string::npos)
                break;
        }
        return out;
    }
    std::string read_one() const {
        char byte;
        auto n = ::read(fd_, &byte, 1);
        socket_check(n >= 0, "read socket byte");
        return n ? std::string(1, byte) : std::string();
    }
    bool would_block() const {
        char byte;
        auto n = ::read(fd_, &byte, 1);
        return n == -1 && (errno == EAGAIN || errno == EWOULDBLOCK);
    }

  private:
    int fd_;
};
inline std::pair<Socket, Socket> socket_pair() {
    int pair[2];
    socket_check(socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, pair) == 0, "socketpair");
    return {Socket(pair[0]), Socket(pair[1])};
}
inline Socket loopback_connect(unsigned port) {
    Socket s(socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0));
    socket_check(s.get() >= 0, "socket");
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = htons(port);
    socket_check(connect(s.get(), reinterpret_cast<sockaddr *>(&a), sizeof(a)) == 0, "connect loopback");
    s.nonblocking();
    return s;
}
inline Socket http_get(unsigned port, const std::string &path) {
    auto s = loopback_connect(port);
    s.send("GET " + path + " HTTP/1.1\r\nHost: localhost\r\n\r\n");
    return s;
}
inline void sleep_ms(int ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }

// Capture libc diagnostics as capfd does; restore stderr before any assertion.
class StderrCapture {
  public:
    StderrCapture() : file_(tmpfile()), saved_(dup(STDERR_FILENO)) {
        socket_check(file_ && saved_ >= 0, "capture stderr");
        fflush(stderr);
        socket_check(dup2(fileno(file_), STDERR_FILENO) >= 0, "redirect stderr");
    }
    ~StderrCapture() {
        if (saved_ >= 0) {
            fflush(stderr);
            dup2(saved_, STDERR_FILENO);
            close(saved_);
        }
        if (file_)
            fclose(file_);
    }
    std::string take() {
        fflush(stderr);
        auto end = ftell(file_);
        fseek(file_, at_, SEEK_SET);
        std::string out(size_t(end - at_), '\0');
        if (!out.empty())
            socket_check(fread(out.data(), 1, out.size(), file_) == out.size(), "read stderr");
        at_ = end;
        fseek(file_, end, SEEK_SET);
        return out;
    }

  private:
    FILE *file_;
    int saved_;
    long at_ = 0;
};
class Env {
  public:
    Env(std::string key, const std::string &value) : key_(std::move(key)) {
        if (const char *old = getenv(key_.c_str())) {
            present_ = true;
            old_ = old;
        }
        socket_check(setenv(key_.c_str(), value.c_str(), 1) == 0, "set fixture environment");
    }
    ~Env() {
        if (present_)
            setenv(key_.c_str(), old_.c_str(), 1);
        else
            unsetenv(key_.c_str());
    }

  private:
    std::string key_, old_;
    bool present_ = false;
};
} // namespace lh
