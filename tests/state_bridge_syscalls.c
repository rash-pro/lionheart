#define _POSIX_C_SOURCE 200809L
#include <arpa/inet.h>
#include <errno.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#ifdef FF8_BRIDGE_PRELOAD
int socket(int domain, int type, int protocol) {
    int pair[2];
    (void)domain; (void)type; (void)protocol;
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, pair)) return -1;
    close(pair[1]); return pair[0];
}
int setsockopt(int fd, int level, int name, const void *value, socklen_t size) {
    (void)fd; (void)level; (void)name; (void)value; (void)size; return 0;
}
int bind(int fd, const struct sockaddr *address, socklen_t size) {
    (void)fd; (void)address; (void)size; errno = EADDRINUSE; return -1;
}
#else
static double clock_value = -1;
static int network_mock, bind_error, reuse_seen, before_bind;
static int clients[32], client_count, client_read, send_mode, sent_once;
static int sync_failure, sync_failures;
static char operations[256];
static size_t operation_count;
void test_clock(double now) { clock_value = now; }
void test_network(int enabled, int error) {
    network_mock = enabled; bind_error = error; reuse_seen = before_bind = 0;
    client_count = client_read = 0;
}
int test_reuse_before_bind(void) { return reuse_seen && before_bind; }
void test_client(int fd) { if (client_count < 32) clients[client_count++] = fd; }
void test_send_mode(int mode) { send_mode = mode; sent_once = 0; }
void test_sync_fail(int kind, int count) { sync_failure = kind; sync_failures = count; }
void test_operations_clear(void) { operation_count = 0; operations[0] = 0; }
const char *test_operations(void) { return operations; }
static void operation(char ch) {
    if (operation_count + 1 < sizeof(operations)) { operations[operation_count++] = ch; operations[operation_count] = 0; }
}
int test_sigpipe_is_default(void) {
    struct sigaction action;
    return sigaction(SIGPIPE, NULL, &action) == 0 && action.sa_handler == SIG_DFL;
}
int __real_clock_gettime(clockid_t, struct timespec *);
int __wrap_clock_gettime(clockid_t id, struct timespec *t) {
    if (id == CLOCK_MONOTONIC && clock_value >= 0) {
        t->tv_sec = (time_t)clock_value; t->tv_nsec = (long)((clock_value - t->tv_sec) * 1000000000.0); return 0;
    }
    return __real_clock_gettime(id, t);
}
int __real_socket(int, int, int);
int __wrap_socket(int domain, int type, int protocol) {
    if (network_mock && domain == AF_INET) {
        int pair[2];
        if (socketpair(AF_UNIX, SOCK_STREAM, 0, pair)) return -1;
        close(pair[1]); return pair[0];
    }
    return __real_socket(domain, type, protocol);
}
int __real_setsockopt(int, int, int, const void *, socklen_t);
int __wrap_setsockopt(int fd, int level, int name, const void *value, socklen_t size) {
    if (network_mock) {
        reuse_seen = level == SOL_SOCKET && name == SO_REUSEADDR && size == sizeof(int) && *(const int *)value == 1;
        return 0;
    }
    return __real_setsockopt(fd, level, name, value, size);
}
int __real_bind(int, const struct sockaddr *, socklen_t);
int __wrap_bind(int fd, const struct sockaddr *address, socklen_t size) {
    if (network_mock) {
        const struct sockaddr_in *a = (const struct sockaddr_in *)address;
        before_bind = reuse_seen && a->sin_addr.s_addr == htonl(INADDR_LOOPBACK);
        if (bind_error) { errno = bind_error; return -1; }
        return 0;
    }
    return __real_bind(fd, address, size);
}
int __real_listen(int, int);
int __wrap_listen(int fd, int count) { return network_mock ? 0 : __real_listen(fd, count); }
int __real_getsockname(int, struct sockaddr *, socklen_t *);
int __wrap_getsockname(int fd, struct sockaddr *address, socklen_t *size) {
    if (network_mock) {
        struct sockaddr_in a;
        memset(&a, 0, sizeof(a)); a.sin_family = AF_INET; a.sin_addr.s_addr = htonl(INADDR_LOOPBACK); a.sin_port = htons(12345);
        memcpy(address, &a, sizeof(a)); *size = sizeof(a); return 0;
    }
    return __real_getsockname(fd, address, size);
}
int __real_accept(int, struct sockaddr *, socklen_t *);
int __wrap_accept(int fd, struct sockaddr *address, socklen_t *size) {
    if (network_mock) {
        if (client_read < client_count) return clients[client_read++];
        errno = EAGAIN; return -1;
    }
    return __real_accept(fd, address, size);
}
ssize_t __real_send(int, const void *, size_t, int);
ssize_t __wrap_send(int fd, const void *data, size_t size, int flags) {
    if (send_mode) {
        if (send_mode == 2 && !sent_once++) return write(fd, data, size < 7 ? size : 7);
        errno = EAGAIN; return -1;
    }
    return __real_send(fd, data, size, flags);
}
int __real_fsync(int);
int __wrap_fsync(int fd) {
    struct stat info;
    int directory = !fstat(fd, &info) && S_ISDIR(info.st_mode);
    operation(directory ? 'D' : 'F');
    if (sync_failures && sync_failure == (directory ? 2 : 1)) { --sync_failures; errno = EIO; return -1; }
    return __real_fsync(fd);
}
int __real_rename(const char *, const char *);
int __wrap_rename(const char *from, const char *to) { operation('R'); return __real_rename(from, to); }
#endif
