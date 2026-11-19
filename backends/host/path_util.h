#ifndef HOST_PATH_UTIL_H
#define HOST_PATH_UTIL_H

#include <errno.h>
#include <fcntl.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#ifdef _WIN32
#include "windows_io.h"
#include <direct.h>
#include <io.h>
#include <process.h>
#else
#include <unistd.h>
#endif

static inline int host_path_is_sep(char c) {
    return c == '/' || c == '\\';
}

static inline int host_path_is_alpha(char c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
}

/* Creates path and all missing parent directories. Handles both '/' and '\',
 * skips Windows drive roots (C:, C:\, C:/), UNC prefixes (\\server\share),
 * and extended prefixes (\\?\...), and routes through mkdir() so Windows
 * uses host_win_mkdir (UTF-8 -> UTF-16 + \\?\). Returns 1 on success, 0 on
 * failure with errno preserved. */
static inline int host_mkdir_p_detail(const char *path, int mode, char *error, size_t error_size) {
    size_t len, start = 0;
    char *copy, *p;
    (void)mode;
    if (error && error_size) error[0] = '\0';
    if (!path || !*path) {
        errno = EINVAL;
        if (error && error_size) snprintf(error, error_size, "no path");
        return 0;
    }
    len = strlen(path);
    copy = (char *)malloc(len + 1);
    if (!copy) {
        if (error && error_size) snprintf(error, error_size, "no path");
        return 0;
    }
    memcpy(copy, path, len + 1);

    if (host_path_is_alpha(copy[0]) && copy[1] == ':') {
        start = 2;
        while (host_path_is_sep(copy[start])) ++start;
    }
#ifdef _WIN32
    else if (host_path_is_sep(copy[0]) && host_path_is_sep(copy[1])) {
#else
    else if (copy[0] == '\\' && copy[1] == '\\') {
#endif
        start = 2;
        if (copy[2] == '?' && host_path_is_sep(copy[3])) {
            start = 4;
            if ((copy[4] == 'U' || copy[4] == 'u') &&
                (copy[5] == 'N' || copy[5] == 'n') &&
                (copy[6] == 'C' || copy[6] == 'c') &&
                host_path_is_sep(copy[7])) {
                start = 8;
                while (copy[start] && !host_path_is_sep(copy[start])) ++start;
                while (host_path_is_sep(copy[start])) ++start;
                while (copy[start] && !host_path_is_sep(copy[start])) ++start;
                while (host_path_is_sep(copy[start])) ++start;
            } else if (host_path_is_alpha(copy[4]) && copy[5] == ':') {
                start = 6;
                while (host_path_is_sep(copy[start])) ++start;
            }
        } else {
            while (host_path_is_sep(copy[start])) ++start;
            while (copy[start] && !host_path_is_sep(copy[start])) ++start;
            while (host_path_is_sep(copy[start])) ++start;
            while (copy[start] && !host_path_is_sep(copy[start])) ++start;
            while (host_path_is_sep(copy[start])) ++start;
        }
    } else {
        while (host_path_is_sep(copy[start])) ++start;
    }

    if (!copy[start]) {
        struct stat st;
        if (stat(copy, &st) != 0) {
            if (error && error_size) snprintf(error, error_size, "cannot read %.4000s: %s", copy, strerror(errno));
            free(copy);
            return 0;
        }
        if (!S_ISDIR(st.st_mode)) {
            errno = ENOTDIR;
            if (error && error_size) snprintf(error, error_size, "cannot use %.4000s: %s", copy, strerror(errno));
            free(copy);
            return 0;
        }
        free(copy);
        return 1;
    }

    p = copy + start;
    for (;;) {
        char saved;
        int made, why;
        struct stat st;
        const char *step = NULL;
        while (*p && !host_path_is_sep(*p)) ++p;
        saved = *p;
        *p = '\0';
        made = (mkdir(copy, (unsigned int)mode) == 0);
        why = errno;
        if (stat(copy, &st) != 0) {
            if (!made) { step = "create"; errno = why; }
            else step = "read";
        } else if (!S_ISDIR(st.st_mode)) {
            step = "use";
            errno = ENOTDIR;
        }
        if (step) {
            if (error && error_size) snprintf(error, error_size, "cannot %s %.4000s: %s", step, copy, strerror(errno));
            free(copy);
            return 0;
        }
        *p = saved;
        while (host_path_is_sep(*p)) ++p;
        if (!*p) break;
    }
    free(copy);
    return 1;
}

static inline int host_mkdir_p(const char *path, int mode) {
    return host_mkdir_p_detail(path, mode, NULL, 0);
}

static inline void host_temp_suffix(char suffix[6], uint32_t attempt) {
    static const char alphabet[] = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz";
    static uint64_t counter = 0;
    uint64_t seq, pid, x;
    int i;
#if defined(__GNUC__) || defined(__clang__)
    seq = __atomic_fetch_add(&counter, (uint64_t)1, __ATOMIC_RELAXED);
#else
    seq = ++counter;
#endif
#ifdef _WIN32
    pid = (uint64_t)(unsigned int)_getpid();
#else
    pid = (uint64_t)(unsigned long)getpid();
#endif
    x = (seq * 0x9e3779b97f4a7c15ULL) ^ (pid << 32) ^
        ((uint64_t)time(NULL) << 16) ^ (uint64_t)clock() ^
        ((uint64_t)(uintptr_t)&seq) ^ ((uint64_t)attempt * 0x517cc1b727220a95ULL);
    x ^= x >> 30;
    x *= 0xbf58476d1ce4e5b9ULL;
    x ^= x >> 27;
    x *= 0x94d049bb133111ebULL;
    x ^= x >> 31;
    for (i = 0; i < 6; ++i) {
        suffix[i] = alphabet[x % 62u];
        x /= 62u;
    }
}

/* Creates a unique temporary file whose path is <prefix><6-alnum> using an
 * O_CREAT|O_EXCL retry loop so Windows routes through host_win_open.
 * Returns the open file descriptor (>= 0) on success, or -1 on failure. */
static inline int host_open_unique_temp(const char *prefix, char *out, size_t out_size, int mode) {
    size_t prefix_len;
    uint32_t attempt;
#ifdef _WIN32
    int flags = O_RDWR | O_CREAT | O_EXCL | O_BINARY;
#elif defined(O_CLOEXEC)
    int flags = O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC;
#else
    int flags = O_RDWR | O_CREAT | O_EXCL;
#endif
    if (!prefix || !out || out_size == 0) {
        errno = EINVAL;
        return -1;
    }
    prefix_len = strlen(prefix);
    if (prefix_len + 7 > out_size) {
        out[0] = '\0';
        errno = ENAMETOOLONG;
        return -1;
    }
    for (attempt = 0; attempt < 256u; ++attempt) {
        char suffix[6];
        int fd;
        host_temp_suffix(suffix, attempt);
        if (out != prefix) memmove(out, prefix, prefix_len);
        memcpy(out + prefix_len, suffix, 6);
        out[prefix_len + 6] = '\0';
        fd = open(out, flags, mode);
        if (fd >= 0) return fd;
        if (errno != EEXIST) {
            out[0] = '\0';
            return -1;
        }
    }
    out[0] = '\0';
    errno = EEXIST;
    return -1;
}

/* Creates a unique temporary directory under <parent> named <prefix><6-alnum>
 * using a mkdir() + EEXIST retry loop so Windows routes through host_win_mkdir.
 * Returns 1 on success, 0 on failure. */
static inline int host_mkdtemp_unique(const char *parent, const char *prefix, char *out, size_t out_size) {
    size_t parent_len, prefix_len, need_sep, base_len;
    uint32_t attempt;
    if (!parent || !*parent || !prefix || !out || out_size == 0) {
        if (out && out_size > 0) out[0] = '\0';
        errno = EINVAL;
        return 0;
    }
    parent_len = strlen(parent);
    prefix_len = strlen(prefix);
    need_sep = host_path_is_sep(parent[parent_len - 1]) ? 0u : 1u;
    base_len = parent_len + need_sep + prefix_len;
    if (base_len + 7 > out_size) {
        out[0] = '\0';
        errno = ENAMETOOLONG;
        return 0;
    }
    for (attempt = 0; attempt < 256u; ++attempt) {
        char suffix[6];
        host_temp_suffix(suffix, attempt);
        memcpy(out, parent, parent_len);
        if (need_sep) out[parent_len] = '/';
        memcpy(out + parent_len + need_sep, prefix, prefix_len);
        memcpy(out + base_len, suffix, 6);
        out[base_len + 6] = '\0';
        if (mkdir(out, 0700) == 0) return 1;
        if (errno != EEXIST) {
            out[0] = '\0';
            return 0;
        }
    }
    out[0] = '\0';
    errno = EEXIST;
    return 0;
}

/* Calls host_win_fopen directly on Windows so C++ files stay UTF-8/long-path
 * safe even after <cstdio> executes #undef fopen. */
static inline FILE *host_fopen(const char *path, const char *mode) {
#ifdef _WIN32
    return host_win_fopen(path, mode);
#else
    return fopen(path, mode);
#endif
}

#endif
