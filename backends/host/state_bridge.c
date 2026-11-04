#define _POSIX_C_SOURCE 200809L
#include "state_bridge.h"
#include "ff8_state.h"
#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <sys/stat.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define CLIENTS 16
#define SNAPSHOT_SIZE 16384
#define QUEUE_SIZE (SNAPSHOT_SIZE * 2)
#define IO_TIMEOUT 5.0
typedef struct {
    int fd, sse, close_after, persistent, ipc_write;
    char request[2048], output[QUEUE_SIZE];
    size_t received, sent, queued;
    double accepted, write_progress;
} client_t;
struct host_state_bridge {
    int listener;
    client_t clients[CLIENTS];
    char state[SNAPSHOT_SIZE], snapshot[SNAPSHOT_SIZE + 128];
    char rich_presence[6200];
    int dirty, rich_dirty;
    double next_publish, next_rich_publish;
};

static double now_seconds(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec / 1000000000.0;
}
static void disconnect(client_t *c) {
    if (c->fd >= 0) close(c->fd);
    memset(c, 0, sizeof(*c)); c->fd = -1;
}
static int nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    return flags >= 0 && fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0 && fcntl(fd, F_SETFD, FD_CLOEXEC) == 0;
}
static void queue(client_t *c, const char *text, double now) {
    size_t n = strlen(text);
    if (c->fd < 0) return;
    if (c->queued == c->sent) c->write_progress = now;
    if (c->sent) {
        memmove(c->output, c->output + c->sent, c->queued - c->sent);
        c->queued -= c->sent; c->sent = 0;
    }
    if (n > sizeof(c->output) - c->queued) { disconnect(c); return; }
    memcpy(c->output + c->queued, text, n); c->queued += n;
}
static void sse(client_t *c, const char *event, const char *json, double now) {
    queue(c, "event: ", now); queue(c, event, now); queue(c, "\ndata: ", now);
    queue(c, json, now); queue(c, "\n\n", now);
}
/* Some IPC sandboxes allow read/write on inherited sockets but reject send.
 * Block/consume only this thread's newly generated SIGPIPE in that fallback;
 * never change the process-wide disposition or consume an older pending signal. */
static ssize_t ipc_write(int fd, const void *data, size_t size) {
    sigset_t mask, old, pending;
    struct timespec zero = {0, 0};
    ssize_t n;
    int before, saved, status;
    sigemptyset(&mask); sigaddset(&mask, SIGPIPE);
    status = pthread_sigmask(SIG_BLOCK, &mask, &old);
    if (status) { errno = status; return -1; }
    if (sigpending(&pending)) {
        saved = errno; pthread_sigmask(SIG_SETMASK, &old, NULL); errno = saved; return -1;
    }
    before = sigismember(&pending, SIGPIPE);
    n = write(fd, data, size); saved = errno;
    if (n < 0 && saved == EPIPE && !before) {
        while (sigtimedwait(&mask, NULL, &zero) < 0 && errno == EINTR) {}
    }
    pthread_sigmask(SIG_SETMASK, &old, NULL); errno = saved;
    return n;
}
static void flush(client_t *c, double now) {
    if (c->fd >= 0 && c->sent < c->queued) {
        ssize_t n;
        if (now - c->write_progress >= IO_TIMEOUT) { disconnect(c); return; }
#ifdef MSG_NOSIGNAL
        if (!c->ipc_write) {
            n = send(c->fd, c->output + c->sent, c->queued - c->sent, MSG_NOSIGNAL);
            if (n < 0 && (errno == EPERM || errno == ENOSYS)) c->ipc_write = 1;
        } else n = -1;
        if (c->ipc_write)
#endif
            n = ipc_write(c->fd, c->output + c->sent, c->queued - c->sent);
        if (n > 0) { c->sent += (size_t)n; c->write_progress = now; }
        else if (!n || (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)) disconnect(c);
    }
    if (c->fd >= 0 && c->sent == c->queued) {
        c->sent = c->queued = 0;
        if (c->close_after) disconnect(c);
    }
}

host_state_bridge_t *host_state_bridge_open(const char *port) {
    host_state_bridge_t *b;
    struct sockaddr_in addr;
    socklen_t size = sizeof(addr);
    char *end;
    long value;
    unsigned i;
    int reuse = 1, inherited = !strncmp(port, "fd:", 3);
    const char *number = inherited ? port + 3 : port;
    errno = 0; value = strtol(number, &end, 10);
    if (errno || !*number || *end || value < 0 || value > (inherited ? 0x7fffffffL : 65535)) {
        fprintf(stderr, "Warning: state bridge disabled: invalid port/fd: %s\n", port); return NULL;
    }
    b = calloc(1, sizeof(*b));
    if (!b) { fprintf(stderr, "Warning: state bridge disabled: allocation failed\n"); return NULL; }
    b->listener = -1;
    for (i = 0; i < CLIENTS; ++i) b->clients[i].fd = -1;
    if (inherited) {
        int fd = dup((int)value);
        struct stat info;
        if (fd < 0 || fstat(fd, &info) || !S_ISSOCK(info.st_mode) || !nonblocking(fd)) {
            if (fd >= 0) close(fd);
            fprintf(stderr, "Warning: state bridge disabled: fd must be a connected stream socket\n");
            host_state_bridge_close(b); return NULL;
        }
        close((int)value); /* Ownership transfers on successful open. */
        b->clients[0].fd = fd; b->clients[0].persistent = 1;
        b->clients[0].accepted = now_seconds();
        fprintf(stderr, "State bridge: inherited socket fd:%ld (HTTP /state, SSE /events, max 4 Hz)\n", value);
        return b;
    }
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET; addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK); addr.sin_port = htons((uint16_t)value);
    b->listener = socket(AF_INET, SOCK_STREAM, 0);
    if (b->listener < 0 || setsockopt(b->listener, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) ||
        !nonblocking(b->listener) || bind(b->listener, (struct sockaddr *)&addr, sizeof(addr)) ||
        listen(b->listener, CLIENTS) || getsockname(b->listener, (struct sockaddr *)&addr, &size)) {
        fprintf(stderr, "Warning: state bridge disabled: %s\n", strerror(errno)); host_state_bridge_close(b); return NULL;
    }
    fprintf(stderr, "State bridge: http://127.0.0.1:%u/state (SSE /events, max 4 Hz)\n", ntohs(addr.sin_port));
    return b;
}

/* No request bodies or pipelining. Reject a buffered tail explicitly instead of
 * dropping bytes and presenting the connection as reusable. */
static int local_host(const char *request) {
    const char *line = strstr(request, "\r\n");
    int hosts = 0;
    if (!line) return 0;
    for (line += 2; *line && strncmp(line, "\r\n", 2);) {
        const char *end = strstr(line, "\r\n"), *colon, *v, *tail;
        size_t key, n;
        char host[80], *port;
        if (!end || *line == ' ' || *line == '\t') return -1;
        colon = memchr(line, ':', (size_t)(end - line));
        if (!colon) return -1;
        key = (size_t)(colon - line); v = colon + 1; tail = end;
        while (v < tail && (*v == ' ' || *v == '\t')) ++v;
        while (tail > v && (tail[-1] == ' ' || tail[-1] == '\t')) --tail;
        if ((key == 17 && !strncasecmp(line, "Transfer-Encoding", key)) ||
            (key == 14 && !strncasecmp(line, "Content-Length", key) && (tail - v != 1 || *v != '0'))) return -1;
        if (key == 4 && !strncasecmp(line, "Host", key)) {
            unsigned value = 0;
            if (++hosts != 1 || (n = (size_t)(tail - v)) >= sizeof(host)) return -1;
            memcpy(host, v, n); host[n] = 0;
            port = strchr(host, ':');
            if (port) {
                *port++ = 0;
                if (!*port) return -1;
                for (; *port; ++port) {
                    if (!isdigit((unsigned char)*port) || value > 6553) return -1;
                    value = value * 10 + (unsigned)(*port - '0');
                    if (value > 65535) return -1;
                }
                if (!value) return -1;
            }
            if (strcasecmp(host, "localhost") && strcmp(host, "127.0.0.1")) return -1;
        }
        line = end + 2;
    }
    return hosts;
}
static void reject(client_t *c, const char *status, double now) {
    char header[160];
    snprintf(header, sizeof(header), "HTTP/1.1 %s\r\nContent-Length: 0\r\nConnection: close\r\n\r\n", status);
    queue(c, header, now); c->close_after = 1;
}
static void request(host_state_bridge_t *b, client_t *c, double now) {
    char *end = strstr(c->request, "\r\n\r\n"), *line;
    int host;
    if (!end) {
        if (c->received == sizeof(c->request) - 1) reject(c, "431 Request Header Fields Too Large", now);
        return;
    }
    if ((size_t)(end + 4 - c->request) != c->received || strlen(c->request) != c->received) {
        reject(c, "400 Bad Request", now); return;
    }
    host = local_host(c->request);
    if (host != 1) { reject(c, host ? "403 Forbidden" : "400 Bad Request", now); return; }
    line = strstr(c->request, "\r\n"); *line = 0;
    if (!strcmp(c->request, "GET /state HTTP/1.1") || !strcmp(c->request, "GET /state HTTP/1.0")) {
        char header[256];
        snprintf(header, sizeof(header), "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nCache-Control: no-store\r\nContent-Length: %zu\r\nConnection: %s\r\n\r\n", strlen(b->snapshot), c->persistent ? "keep-alive" : "close");
        queue(c, header, now); queue(c, b->snapshot, now); c->close_after = !c->persistent;
        c->received = 0; c->request[0] = 0;
    } else if (!strcmp(c->request, "GET /events HTTP/1.1") || !strcmp(c->request, "GET /events HTTP/1.0")) {
        queue(c, "HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\nCache-Control: no-store\r\nConnection: close\r\n\r\n", now);
        if (c->fd >= 0) { c->sse = 1; sse(c, "state", b->snapshot, now); }
    } else reject(c, "404 Not Found", now);
}
static void publish_rich_presence(host_state_bridge_t *b, double now) {
    unsigned i;
    if (!b->rich_dirty || now < b->next_rich_publish) return;
    for (i = 0; i < CLIENTS; ++i) if (b->clients[i].sse) sse(&b->clients[i], "achievement", b->rich_presence, now);
    b->rich_dirty = 0; b->next_rich_publish = now + 0.25;
}

void host_state_bridge_restore(host_state_bridge_t *b, const cpu_t *c, uint64_t vblank) {
    b->dirty=1; b->next_publish=0;
    host_state_bridge_frame(b,c,vblank);
}

void host_state_bridge_frame(host_state_bridge_t *b, const cpu_t *cpu, uint64_t vblank) {
    char state[SNAPSHOT_SIZE];
    double now = now_seconds();
    unsigned i, slot;
    int accepted;
    host_ff8_state_json(cpu, state, sizeof(state));
    if (strcmp(state, b->state)) { memcpy(b->state, state, strlen(state) + 1); b->dirty = 1; }
    snprintf(b->snapshot, sizeof(b->snapshot), "{\"schema\":1,\"vblank\":%llu,\"state\":%s}", (unsigned long long)vblank, state);
    if (b->dirty && now >= b->next_publish) {
        for (i = 0; i < CLIENTS; ++i) if (b->clients[i].sse) sse(&b->clients[i], "state", b->snapshot, now);
        b->dirty = 0; b->next_publish = now + 0.25;
    }
    publish_rich_presence(b, now);
    for (i = 0; b->listener >= 0 && i < CLIENTS; ++i) {
        accepted = accept(b->listener, NULL, NULL);
        if (accepted < 0) break;
        if (!nonblocking(accepted)) { close(accepted); continue; }
        for (slot = 0; slot < CLIENTS && b->clients[slot].fd >= 0; ++slot) {}
        if (slot == CLIENTS) { close(accepted); continue; }
        b->clients[slot].fd = accepted; b->clients[slot].accepted = now;
    }
    for (i = 0; i < CLIENTS; ++i) {
        client_t *c = &b->clients[i];
        if (c->fd < 0) continue;
        if (!c->sse && !c->close_after) {
            ssize_t n = read(c->fd, c->request + c->received, sizeof(c->request) - c->received - 1);
            if (n > 0) {
                c->received += (size_t)n; c->request[c->received] = 0;
                request(b, c, now);
            } else if (!n || (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)) disconnect(c);
            if (c->fd >= 0 && !c->persistent && !c->close_after && now - c->accepted >= IO_TIMEOUT) disconnect(c);
        } else if (c->sse) {
            char byte;
            ssize_t n = read(c->fd, &byte, 1);
            if (n >= 0 || (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)) disconnect(c);
        }
        flush(c, now);
    }
}

void host_state_bridge_event(void *context, const char *json) {
    host_state_bridge_t *b = context;
    double now;
    unsigned i;
    if (!b) return;
    now = now_seconds();
    if (!strncmp(json, "{\"type\":\"rich_presence\",", 24)) {
        if (strlen(json) >= sizeof(b->rich_presence)) return;
        strcpy(b->rich_presence, json); b->rich_dirty = 1;
        publish_rich_presence(b, now);
    } else for (i = 0; i < CLIENTS; ++i) if (b->clients[i].sse) sse(&b->clients[i], "achievement", json, now);
    for (i = 0; i < CLIENTS; ++i) flush(&b->clients[i], now);
}
void host_state_bridge_close(host_state_bridge_t *b) {
    unsigned i;
    if (!b) return;
    for (i = 0; i < CLIENTS; ++i) disconnect(&b->clients[i]);
    if (b->listener >= 0) close(b->listener);
    free(b);
}
