#if !defined(_WIN32) && !defined(_GNU_SOURCE)
/* CPU affinity, vfork and per-thread priority are Linux interfaces. */
#define _GNU_SOURCE
#endif
#include "native_build.h"

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <dirent.h>
#include <fcntl.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <sys/file.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>
extern char **environ;
#endif

#define PATH_CAP 4096
/* What one compiler process may take, from the measurements: about 1 GiB at
 * the worst set, plus the driver. */
#define COMPILER_BYTES (1100ull << 20)
/* The diagnostics kept for a failed set: enough to read the error. */
#define TAIL_CAP 1536

/* ---- platform ---- */

#ifdef _WIN32
typedef SRWLOCK lock_t;
typedef CONDITION_VARIABLE cond_t;
typedef HANDLE thread_t;
typedef HANDLE file_lock_t;
#define NO_FILE_LOCK NULL
static void lock_init(lock_t *l) { InitializeSRWLock(l); }
static void lock(lock_t *l) { AcquireSRWLockExclusive(l); }
static void unlock(lock_t *l) { ReleaseSRWLockExclusive(l); }
static void cond_init(cond_t *c) { InitializeConditionVariable(c); }
static void cond_wait(cond_t *c, lock_t *l, unsigned ms) { SleepConditionVariableSRW(c, l, ms ? ms : INFINITE, 0); }
static void cond_all(cond_t *c) { WakeAllConditionVariable(c); }
static double now(void) {
    LARGE_INTEGER t, f;
    QueryPerformanceCounter(&t); QueryPerformanceFrequency(&f);
    return (double)t.QuadPart / (double)f.QuadPart;
}
static wchar_t *wide(const char *text) {
    int count = MultiByteToWideChar(CP_UTF8, 0, text, -1, NULL, 0);
    wchar_t *out = count > 0 ? malloc((size_t)count * sizeof(*out)) : NULL;
    if (out && !MultiByteToWideChar(CP_UTF8, 0, text, -1, out, count)) { free(out); out = NULL; }
    return out;
}
#else
typedef pthread_mutex_t lock_t;
typedef pthread_cond_t cond_t;
typedef pthread_t thread_t;
typedef int file_lock_t;
#define NO_FILE_LOCK (-1)
static void lock_init(lock_t *l) { pthread_mutex_init(l, NULL); }
static void lock(lock_t *l) { pthread_mutex_lock(l); }
static void unlock(lock_t *l) { pthread_mutex_unlock(l); }
static void cond_init(cond_t *c) {
    pthread_condattr_t a;
    pthread_condattr_init(&a);
    pthread_condattr_setclock(&a, CLOCK_MONOTONIC);
    pthread_cond_init(c, &a);
    pthread_condattr_destroy(&a);
}
static void cond_wait(cond_t *c, lock_t *l, unsigned ms) {
    struct timespec t;
    if (!ms) { pthread_cond_wait(c, l); return; }
    clock_gettime(CLOCK_MONOTONIC, &t);
    t.tv_sec += ms / 1000; t.tv_nsec += (long)(ms % 1000) * 1000000L;
    if (t.tv_nsec >= 1000000000L) { t.tv_sec++; t.tv_nsec -= 1000000000L; }
    pthread_cond_timedwait(c, l, &t);
}
static void cond_all(cond_t *c) { pthread_cond_broadcast(c); }
static double now(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec + (double)t.tv_nsec / 1e9;
}
#endif

/* Processor time of this process and the compilers it has waited for. */
static double cpu_seconds(void) {
#ifdef _WIN32
    FILETIME created, ended, kernel, user;
    if (!GetProcessTimes(GetCurrentProcess(), &created, &ended, &kernel, &user)) return -1;
    /* Compilers run in their own processes and are not counted here. */
    return ((double)((uint64_t)kernel.dwHighDateTime << 32 | kernel.dwLowDateTime) +
            (double)((uint64_t)user.dwHighDateTime << 32 | user.dwLowDateTime)) / 1e7;
#else
    struct rusage self, children;
    getrusage(RUSAGE_SELF, &self);
    getrusage(RUSAGE_CHILDREN, &children);
    return (double)(self.ru_utime.tv_sec + self.ru_stime.tv_sec + children.ru_utime.tv_sec + children.ru_stime.tv_sec) +
           (double)(self.ru_utime.tv_usec + self.ru_stime.tv_usec + children.ru_utime.tv_usec + children.ru_stime.tv_usec) / 1e6;
#endif
}

/* Paths that do not fit are refused, never cut short. */
static int path_of(char *out, const char *format, ...) {
    va_list arguments;
    int n;
    va_start(arguments, format);
    n = vsnprintf(out, PATH_CAP, format, arguments);
    va_end(arguments);
    return n > 0 && n < PATH_CAP;
}

static int join(char *out, const char *a, const char *b) { return path_of(out, "%s/%s", a, b); }

static int make_directory(const char *path) {
#ifdef _WIN32
    wchar_t *w = wide(path);
    int ok = w && (CreateDirectoryW(w, NULL) || GetLastError() == ERROR_ALREADY_EXISTS);
    free(w);
    return ok;
#else
    return !mkdir(path, 0755) || errno == EEXIST;
#endif
}

static int exists(const char *path, uint64_t *size) {
#ifdef _WIN32
    WIN32_FILE_ATTRIBUTE_DATA a;
    wchar_t *w = wide(path);
    int ok = w && GetFileAttributesExW(w, GetFileExInfoStandard, &a);
    free(w);
    if (ok && size) *size = (uint64_t)a.nFileSizeHigh << 32 | a.nFileSizeLow;
    return ok;
#else
    struct stat s;
    if (stat(path, &s)) return 0;
    if (size) *size = (uint64_t)s.st_size;
    return 1;
#endif
}

static int remove_file(const char *path) {
#ifdef _WIN32
    wchar_t *w = wide(path);
    int ok = w && (DeleteFileW(w) || GetLastError() == ERROR_FILE_NOT_FOUND);
    free(w);
    return ok;
#else
    return !unlink(path) || errno == ENOENT;
#endif
}

typedef void (*visit_fn)(void *user, const char *name, int directory);
static int list(const char *path, visit_fn visit, void *user) {
#ifdef _WIN32
    char pattern[PATH_CAP];
    wchar_t *w;
    WIN32_FIND_DATAW found;
    HANDLE find;
    if (!join(pattern, path, "*") || !(w = wide(pattern))) return 0;
    find = FindFirstFileW(w, &found);
    free(w);
    if (find == INVALID_HANDLE_VALUE) return GetLastError() == ERROR_FILE_NOT_FOUND;
    do {
        char name[1024];
        if (!wcscmp(found.cFileName, L".") || !wcscmp(found.cFileName, L"..")) continue;
        if (!WideCharToMultiByte(CP_UTF8, 0, found.cFileName, -1, name, sizeof(name), NULL, NULL)) continue;
        visit(user, name, !!(found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY));
    } while (FindNextFileW(find, &found));
    FindClose(find);
    return 1;
#else
    DIR *d = opendir(path);
    struct dirent *row;
    if (!d) return errno == ENOENT;
    while ((row = readdir(d))) {
        char full[PATH_CAP];
        struct stat s;
        if (!strcmp(row->d_name, ".") || !strcmp(row->d_name, "..")) continue;
        if (!join(full, path, row->d_name)) continue;
        visit(user, row->d_name, !lstat(full, &s) && S_ISDIR(s.st_mode));
    }
    closedir(d);
    return 1;
#endif
}

typedef struct { const char *parent; int ok; } tree_t;
static int remove_tree(const char *path);
static void remove_entry(void *user, const char *name, int directory) {
    tree_t *t = user;
    char full[PATH_CAP];
    if (!join(full, t->parent, name)) { t->ok = 0; return; }
    if (!(directory ? remove_tree(full) : remove_file(full))) t->ok = 0;
}
static int remove_tree(const char *path) {
    tree_t t = {path, 1};
    if (!list(path, remove_entry, &t)) return 0;
#ifdef _WIN32
    {
        wchar_t *w = wide(path);
        int ok = w && (RemoveDirectoryW(w) || GetLastError() == ERROR_FILE_NOT_FOUND || GetLastError() == ERROR_PATH_NOT_FOUND);
        free(w);
        return t.ok && ok;
    }
#else
    return t.ok && (!rmdir(path) || errno == ENOENT);
#endif
}

/* Durable before visible: a receipt must never name bytes still in flight. */
static int write_file(const char *path, const void *data, size_t size) {
#ifdef _WIN32
    wchar_t *w = wide(path);
    HANDLE f = w ? CreateFileW(w, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL) : INVALID_HANDLE_VALUE;
    DWORD written = 0;
    int ok = f != INVALID_HANDLE_VALUE && WriteFile(f, data, (DWORD)size, &written, NULL) && written == size && FlushFileBuffers(f);
    free(w);
    if (f != INVALID_HANDLE_VALUE) ok = CloseHandle(f) && ok;
    return ok;
#else
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    const char *p = data;
    int ok = fd >= 0;
    while (ok && size) {
        ssize_t n = write(fd, p, size);
        if (n <= 0) { ok = 0; break; }
        p += n; size -= (size_t)n;
    }
    if (fd >= 0) { ok = !fsync(fd) && ok; ok = !close(fd) && ok; }
    return ok;
#endif
}

static int sync_file(const char *path) {
#ifdef _WIN32
    wchar_t *w = wide(path);
    HANDLE f = w ? CreateFileW(w, GENERIC_WRITE, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL) : INVALID_HANDLE_VALUE;
    int ok = f != INVALID_HANDLE_VALUE && FlushFileBuffers(f);
    free(w);
    if (f != INVALID_HANDLE_VALUE) CloseHandle(f);
    return ok;
#else
    int fd = open(path, O_RDONLY | O_CLOEXEC), ok;
    if (fd < 0) return 0;
    ok = !fsync(fd);
    return !close(fd) && ok;
#endif
}

static void sync_directory(const char *path) {
#ifndef _WIN32
    int fd = open(path, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd >= 0) { (void)fsync(fd); close(fd); }
#else
    (void)path;
#endif
}

static int rename_directory(const char *from, const char *to) {
#ifdef _WIN32
    wchar_t *a = wide(from), *b = wide(to);
    int ok = a && b && MoveFileExW(a, b, 0);
    free(a); free(b);
    return ok;
#else
    return !rename(from, to);
#endif
}

static int is_key(const char *name) {
    size_t i;
    for (i = 0; i < 64; ++i) if (!((name[i] >= '0' && name[i] <= '9') || (name[i] >= 'a' && name[i] <= 'f'))) return 0;
    return !name[64];
}

/* replace: a receipt may replace an older one; a module never replaces anything. */
static int publish(const char *from, const char *to, int replace) {
#ifdef _WIN32
    wchar_t *a = wide(from), *b = wide(to);
    int ok = a && b && MoveFileExW(a, b, (replace ? MOVEFILE_REPLACE_EXISTING : 0) | MOVEFILE_WRITE_THROUGH);
    free(a); free(b);
    return ok;
#else
    if (replace) return !rename(from, to);
    /* link fails rather than replace, which rename cannot promise everywhere. */
    if (!link(from, to)) return !unlink(from) || 1;
    if (errno == EEXIST) return 0;
    return !rename(from, to);
#endif
}

static int write_atomic(const char *path, const void *data, size_t size) {
    char temporary[PATH_CAP];
    int n = snprintf(temporary, sizeof(temporary), "%s.tmp", path);
    if (n < 0 || n >= (int)sizeof(temporary)) return 0;
    if (!write_file(temporary, data, size) || !publish(temporary, path, 1)) { remove_file(temporary); return 0; }
    return 1;
}

static char *read_small(const char *path, size_t limit, size_t *length) {
    FILE *f;
    char *data = malloc(limit + 1);
    size_t n;
#ifdef _WIN32
    wchar_t *w = wide(path);
    f = w ? _wfopen(w, L"rb") : NULL;
    free(w);
#else
    f = fopen(path, "rb");
#endif
    if (!f || !data) { if (f) fclose(f); free(data); return NULL; }
    n = fread(data, 1, limit, f);
    fclose(f);
    data[n] = 0;
    if (length) *length = n;
    return data;
}

/* Advisory locks: exclusive for the builder, shared for a folder in use. The
 * kernel drops them with the process, so a killed player holds nothing. */
static file_lock_t file_lock(const char *path, int exclusive, int wait) {
#ifdef _WIN32
    OVERLAPPED at;
    wchar_t *w = wide(path);
    HANDLE f = w ? CreateFileW(w, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_ALWAYS,
                               FILE_ATTRIBUTE_NORMAL, NULL) : INVALID_HANDLE_VALUE;
    free(w);
    if (f == INVALID_HANDLE_VALUE) return NULL;
    memset(&at, 0, sizeof(at));
    if (!LockFileEx(f, (exclusive ? LOCKFILE_EXCLUSIVE_LOCK : 0) | (wait ? 0 : LOCKFILE_FAIL_IMMEDIATELY), 0, 1, 0, &at)) {
        CloseHandle(f); return NULL;
    }
    return f;
#else
    for (;;) {
        struct stat held, named;
        int fd = open(path, O_RDWR | O_CREAT | O_CLOEXEC, 0644);
        if (fd < 0) return -1;
        if (flock(fd, (exclusive ? LOCK_EX : LOCK_SH) | (wait ? 0 : LOCK_NB))) { close(fd); return -1; }
        /* A remover may have unlinked the file while this waited for it. */
        if (!fstat(fd, &held) && !stat(path, &named) && held.st_ino == named.st_ino && held.st_dev == named.st_dev) return fd;
        close(fd);
    }
#endif
}

static void file_unlock(file_lock_t l) {
#ifdef _WIN32
    if (l) CloseHandle(l);
#else
    if (l >= 0) close(l);
#endif
}

static unsigned processors(void) {
#ifdef _WIN32
    DWORD n = GetActiveProcessorCount(ALL_PROCESSOR_GROUPS);
    return n ? (unsigned)n : 1;
#else
    cpu_set_t set;
    long n;
    if (!sched_getaffinity(0, sizeof(set), &set) && CPU_COUNT(&set) > 0) return (unsigned)CPU_COUNT(&set);
    n = sysconf(_SC_NPROCESSORS_ONLN);
    return n > 0 ? (unsigned)n : 1;
#endif
}

static uint64_t available_memory(void) {
#ifdef _WIN32
    MEMORYSTATUSEX m;
    m.dwLength = sizeof(m);
    return GlobalMemoryStatusEx(&m) ? m.ullAvailPhys : 0;
#else
    char *text = read_small("/proc/meminfo", 8192, NULL), *at;
    uint64_t kib = 0;
    if (text && (at = strstr(text, "MemAvailable:"))) kib = strtoull(at + 13, NULL, 10);
    free(text);
    return kib << 10;
#endif
}

/* ---- processes ---- */

typedef struct {
#ifdef _WIN32
    HANDLE process;
#else
    pid_t pid;
#endif
    int active;
} child_t;

#ifndef _WIN32
/* The death signal below reaches only the driver, and zig cc runs the real
 * compiler as a child of its own, which would outlive a player that crashes
 * or is killed. One small process, forked when building starts, holds the
 * read end of a pipe on which every compiler announces its process group
 * before it runs. Whatever ends the player closes the write end, and that
 * process then kills every group still announced. As the child of a threaded
 * process it touches only memory it was given and makes only system calls. */
#define REAPER_GROUPS 4096
static pid_t reaper_pid;
static int reaper_fd = -1;
static int32_t reaper_groups[REAPER_GROUPS];

static void reaper_main(int fd) {
    struct sigaction ignore;
    size_t count = 0, k;
    int32_t message[256];
    ssize_t got, i;
    int other;
    memset(&ignore, 0, sizeof(ignore));
    ignore.sa_handler = SIG_IGN;
    /* Only the pipe ends it: signals a terminal sends to the player's group
     * must not take it first and leave the compilers running. */
    sigaction(SIGINT, &ignore, NULL); sigaction(SIGTERM, &ignore, NULL);
    sigaction(SIGHUP, &ignore, NULL); sigaction(SIGQUIT, &ignore, NULL);
    sigaction(SIGPIPE, &ignore, NULL);
    setpgid(0, 0);
    prctl(PR_SET_NAME, "lionheart-cc", 0, 0, 0);
    if (fd) dup2(fd, 0);
    /* Hold nothing else of the player's: its log pipe must close when it does. */
#ifdef SYS_close_range
    if (syscall(SYS_close_range, 1u, ~0u, 0u))
#endif
        for (other = 1; other < 4096; ++other) close(other);
    for (;;) {
        got = read(0, message, sizeof(message));
        if (got < 0 && errno == EINTR) continue;
        if (got <= 0) break;
        for (i = 0; i < got / (ssize_t)sizeof(message[0]); ++i) {
            if (message[i] > 0 && count < REAPER_GROUPS) reaper_groups[count++] = message[i];
            else if (message[i] < 0)
                for (k = 0; k < count; ++k)
                    if (reaper_groups[k] == -message[i]) { reaper_groups[k] = reaper_groups[--count]; break; }
        }
    }
    for (k = 0; k < count; ++k) kill(-reaper_groups[k], SIGKILL);
    _exit(0);
}

static void reaper_start(void) {
    int fds[2];
    pid_t pid;
    if (pipe2(fds, O_CLOEXEC)) return;
    pid = fork();
    if (!pid) reaper_main(fds[0]);
    close(fds[0]);
    if (pid < 0) { close(fds[1]); return; }
    reaper_pid = pid;
    reaper_fd = fds[1];
}

/* Positive announces a group, negative retires it. A write this small is
 * atomic on a pipe, so workers need no lock around it. */
static void reaper_note(int32_t group) {
    ssize_t written;
    if (reaper_fd >= 0) { written = write(reaper_fd, &group, sizeof(group)); (void)written; }
}

/* Every compiler has ended by now, so it kills nothing. */
static void reaper_stop(void) {
    if (reaper_fd < 0) return;
    close(reaper_fd);
    reaper_fd = -1;
    while (waitpid(reaper_pid, NULL, 0) < 0 && errno == EINTR) {}
}

/* The child shares this thread's memory until exec and only makes system
 * calls; it inherits this worker's lower priority, and dies with it. Its own
 * process group lets a stop take the driver's own children with it, and the
 * group is announced from the child itself, before the driver can start
 * any, so no moment exists in which a crash would miss them. */
static pid_t spawn(const char *const *argv, char **envp, int fd, pid_t player) {
    pid_t pid = vfork();
    if (!pid) {
        setpgid(0, 0);
        prctl(PR_SET_PDEATHSIG, SIGKILL);
        /* The player may have died before the signal was asked for. */
        if (getppid() != player) _exit(127);
        reaper_note((int32_t)getpid());
        dup2(fd, 1); dup2(fd, 2);
        execve(argv[0], (char *const *)argv, envp);
        _exit(127);
    }
    return pid;
}
#endif

/* 0 ran (exit code in *code), -1 could not start. */
static int run(const char *const *argv, const char *const *env, const char *log, child_t *child, lock_t *guard, int *code) {
#ifdef _WIN32
    size_t length = 1, i;
    wchar_t *line, *block = NULL, *w;
    char *text;
    STARTUPINFOEXW si;
    PROCESS_INFORMATION pi;
    SIZE_T attributes_size = 0;
    LPPROC_THREAD_ATTRIBUTE_LIST attributes = NULL;
    SECURITY_ATTRIBUTES inherit = {sizeof(inherit), NULL, TRUE};
    HANDLE out;
    DWORD exit_code = 1;
    for (i = 0; argv[i]; ++i) length += strlen(argv[i]) * 2 + 3;
    if (!(text = malloc(length))) return -1;
    text[0] = 0;
    /* The quoting CommandLineToArgvW undoes. */
    for (i = 0; argv[i]; ++i) {
        const char *p = argv[i];
        char *o = text + strlen(text);
        if (i) *o++ = ' ';
        *o++ = '"';
        for (; *p; ++p) {
            size_t slashes = 0;
            while (p[slashes] == '\\') ++slashes;
            if (p[slashes] == '"' || !p[slashes]) {
                size_t k, n = p[slashes] ? slashes * 2 + 1 : slashes * 2;
                for (k = 0; k < n; ++k) *o++ = '\\';
                p += slashes;
                if (!*p) { --p; break; }
                *o++ = '"';
            } else { size_t k; for (k = 0; k < slashes; ++k) *o++ = '\\'; p += slashes; *o++ = *p; }
        }
        *o++ = '"';
        *o = 0;
    }
    line = wide(text);
    free(text);
    if (!line) return -1;
    {
        wchar_t *current = GetEnvironmentStringsW(), *p;
        size_t used = 0, extra = 0, total;
        for (p = current; *p; p += wcslen(p) + 1) used += wcslen(p) + 1;
        for (i = 0; env[i]; ++i) extra += strlen(env[i]) * 2 + 2;
        total = used + extra + 1;
        block = malloc(total * sizeof(wchar_t));
        if (block) {
            size_t at = 0;
            memcpy(block, current, used * sizeof(wchar_t)); at = used;
            for (i = 0; env[i]; ++i) {
                int n = MultiByteToWideChar(CP_UTF8, 0, env[i], -1, block + at, (int)(total - at));
                at += n > 0 ? (size_t)n : 0;
            }
            block[at] = 0;
        }
        FreeEnvironmentStringsW(current);
    }
    w = wide(log);
    out = w ? CreateFileW(w, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_DELETE, &inherit, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL)
            : INVALID_HANDLE_VALUE;
    free(w);
    memset(&si, 0, sizeof(si));
    si.StartupInfo.cb = sizeof(si);
    si.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    si.StartupInfo.hStdInput = NULL; si.StartupInfo.hStdOutput = out; si.StartupInfo.hStdError = out;
    /* Only this log: a child inheriting every open handle would keep other
     * workers' build folders from being removed until it exits. */
    InitializeProcThreadAttributeList(NULL, 1, 0, &attributes_size);
    attributes = attributes_size ? malloc(attributes_size) : NULL;
    if (attributes && (!InitializeProcThreadAttributeList(attributes, 1, 0, &attributes_size) ||
                       !UpdateProcThreadAttribute(attributes, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, &out, sizeof(out), NULL, NULL))) {
        free(attributes); attributes = NULL;
    }
    si.lpAttributeList = attributes;
    lock(guard);
    if (child->active < 0 || !block || !attributes || out == INVALID_HANDLE_VALUE ||
        !CreateProcessW(NULL, line, NULL, NULL, TRUE,
                        CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT | BELOW_NORMAL_PRIORITY_CLASS | CREATE_SUSPENDED | EXTENDED_STARTUPINFO_PRESENT,
                        block, NULL, &si.StartupInfo, &pi)) {
        unlock(guard);
        free(line); free(block);
        if (attributes) { DeleteProcThreadAttributeList(attributes); free(attributes); }
        if (out != INVALID_HANDLE_VALUE) CloseHandle(out);
        return -1;
    }
    DeleteProcThreadAttributeList(attributes);
    free(attributes);
    {
        extern HANDLE native_build_job;
        if (native_build_job) AssignProcessToJobObject(native_build_job, pi.hProcess);
    }
    ResumeThread(pi.hThread);
    CloseHandle(pi.hThread);
    child->process = pi.hProcess; child->active = 1;
    unlock(guard);
    free(line); free(block); CloseHandle(out);
    WaitForSingleObject(pi.hProcess, INFINITE);
    GetExitCodeProcess(pi.hProcess, &exit_code);
    lock(guard);
    child->active = child->active < 0 ? -1 : 0;
    CloseHandle(pi.hProcess); child->process = NULL;
    unlock(guard);
    *code = (int)exit_code;
    return 0;
#else
    size_t n = 0, e = 0, i;
    char **envp;
    int fd, status;
    pid_t pid;
    for (; environ[n]; ++n) {}
    for (; env[e]; ++e) {}
    if (!(envp = malloc((n + e + 1) * sizeof(*envp)))) return -1;
    /* The child's own variables first: the C library takes the first match. */
    for (i = 0; i < e; ++i) envp[i] = (char *)env[i];
    for (i = 0; i < n; ++i) envp[e + i] = environ[i];
    envp[n + e] = NULL;
    fd = open(log, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0) { free(envp); return -1; }
    lock(guard);
    if (child->active < 0) { unlock(guard); close(fd); free(envp); return -1; }
    pid = spawn(argv, envp, fd, getpid());
    if (pid > 0) { child->pid = pid; child->active = 1; }
    unlock(guard);
    close(fd); free(envp);
    if (pid < 0) return -1;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
    reaper_note(-(int32_t)pid);
    lock(guard);
    child->active = child->active < 0 ? -1 : 0;
    unlock(guard);
    if (WIFEXITED(status) && WEXITSTATUS(status) == 127) return -1;
    *code = WIFEXITED(status) ? WEXITSTATUS(status) : 128 + (WIFSIGNALED(status) ? WTERMSIG(status) : 0);
    return 0;
#endif
}

#ifdef _WIN32
HANDLE native_build_job;
#endif

/* ---- the build ---- */

/* DELIVERING: its module exists and is being handed to the loader. */
typedef enum { SET_PENDING, SET_EMITTING, SET_COMPILING, SET_DELIVERING, SET_READY, SET_FAILED } set_state_t;

typedef struct job job_t;
typedef struct {
    native_build_set_t info;
    set_state_t state;
    job_t *job;
    unsigned attempts;
    uint32_t promoted;
    char key[65];
} set_t;

struct job {
    job_t *next;
    char key[65], directory[PATH_CAP];
    size_t *sets, set_count;
    char **units;
    size_t unit_count, started, finished;
    int failed, linking;
    char tail[TAIL_CAP];
    uint64_t work;
};

typedef enum { KNOWN_NEW, KNOWN_DELIVERING, KNOWN_DELIVERED, KNOWN_REFUSED } known_state_t;
typedef struct {
    char key[65];
    known_state_t state;
} known_t;

#define RING 64
typedef struct {
    uint32_t pc;
    uint8_t bytes[64];
    uint8_t size;
} sample_t;

/* One image can be a set on each of four discs; code that more sets share is
 * library code every effect carries, and says nothing about which one runs. */
#define SPECIFIC_MATCHES 4

static struct {
    int running, stopping, builder, source_open;
    native_build_config_t config;
    native_build_source_t source;
    char root[PATH_CAP], cache[PATH_CAP], include[PATH_CAP], modules[PATH_CAP], sets_dir[PATH_CAP], failed_dir[PATH_CAP], work[PATH_CAP];
    char **driver, **files_data_names;
    lock_t guard, emit_guard;
    cond_t changed;
    thread_t service, *workers;
    child_t *children;
    unsigned worker_count;
    file_lock_t in_use, builder_lock;
    set_t *sets;
    size_t set_count;
    job_t *jobs;
    known_t *known;
    size_t known_count, known_capacity;
    unsigned disc;
    uint32_t promotions;
    unsigned sequence;
    native_build_status_t status;
    double start, compile_start, start_cpu;
    uint64_t compiled_work;
    /* Lock-free ring from the guest thread. */
    sample_t ring[RING];
    unsigned ring_write, ring_read;
} b;

static void say(const char *format, ...) {
    va_list arguments;
    if (!b.config.verbose) return;
    va_start(arguments, format);
    fprintf(stderr, "native: ");
    vfprintf(stderr, format, arguments);
    fputc('\n', stderr);
    va_end(arguments);
}

/* Called with the guard held. The first error is the one that explains. */
static void stop_building(const char *format, ...) {
    va_list arguments;
    if (b.status.state != NATIVE_BUILD_STOPPED) {
        va_start(arguments, format);
        vsnprintf(b.status.error, sizeof(b.status.error), format, arguments);
        va_end(arguments);
        b.status.state = NATIVE_BUILD_STOPPED;
        fprintf(stderr, "native: building stopped: %s\n", b.status.error);
    }
    cond_all(&b.changed);
}

static void set_error(const char *format, ...) {
    va_list arguments;
    va_start(arguments, format);
    vsnprintf(b.status.error, sizeof(b.status.error), format, arguments);
    va_end(arguments);
}

/* Lower sorts first: the executable, sets the guest ran, the inserted disc's
 * game sets, its battle effects, then everything else. */
static uint64_t priority(const set_t *s) {
    unsigned tier, here = b.disc && (s->info.discs & (1u << b.disc));
    if (s->info.index < 0) tier = 0;
    else if (s->promoted) return (uint64_t)1 << 40 | (UINT32_MAX - s->promoted);
    else if (here && !s->info.effect) tier = 2;
    else if (here) tier = 3;
    else if (!s->info.effect) tier = 4;
    else tier = 5;
    return (uint64_t)tier << 40 | (uint32_t)(s - b.sets);
}

static uint64_t job_priority(const job_t *j) {
    uint64_t best = UINT64_MAX;
    size_t i;
    for (i = 0; i < j->set_count; ++i) {
        uint64_t p = priority(&b.sets[j->sets[i]]);
        if (p < best) best = p;
    }
    return best;
}

static known_t *find_known(const char *key) {
    size_t i;
    for (i = 0; i < b.known_count; ++i) if (!strcmp(b.known[i].key, key)) return &b.known[i];
    return NULL;
}

static known_t *add_known(const char *key) {
    known_t *k = find_known(key);
    if (k) return k;
    if (b.known_count == b.known_capacity) {
        size_t capacity = b.known_capacity ? b.known_capacity * 2 : 64;
        known_t *grown = realloc(b.known, capacity * sizeof(*grown));
        if (!grown) return NULL;
        b.known = grown; b.known_capacity = capacity;
    }
    k = &b.known[b.known_count++];
    memset(k, 0, sizeof(*k));
    snprintf(k->key, sizeof(k->key), "%s", key);
    return k;
}

static const char *suffix(void) { return b.config.windows ? ".dll" : ".so"; }

static int module_path(char *out, const char *key, const char *extension) {
    int n = snprintf(out, PATH_CAP, "%s/%s%s", b.modules, key, extension);
    return n > 0 && n < PATH_CAP;
}

/* A module counts only with its receipt, and the receipt only if the module
 * it describes is still whole. */
static int module_valid(const char *key) {
    char path[PATH_CAP], *text;
    uint64_t size = 0, recorded;
    int ok;
    if (!module_path(path, key, ".ok") || !(text = read_small(path, 256, NULL))) return 0;
    ok = sscanf(text, "lionheart-module 1 size %llu", (unsigned long long *)&recorded) == 1;
    free(text);
    return ok && module_path(path, key, suffix()) && exists(path, &size) && size == recorded;
}

static int read_receipt(const set_t *s, char key[65]) {
    char path[PATH_CAP], *text;
    size_t length, i;
    int ok;
    if (!join(path, b.sets_dir, s->info.identity) || !(text = read_small(path, 128, &length))) return 0;
    for (i = 0; i < 64 && ((text[i] >= '0' && text[i] <= '9') || (text[i] >= 'a' && text[i] <= 'f')); ++i) {}
    ok = i == 64;
    if (ok) { memcpy(key, text, 64); key[64] = 0; }
    free(text);
    return ok;
}

static unsigned read_failure(const set_t *s) {
    char path[PATH_CAP], *text;
    unsigned attempts = 0;
    if (!join(path, b.failed_dir, s->info.identity) || !(text = read_small(path, 64, NULL))) return 0;
    if (sscanf(text, "attempts %u", &attempts) != 1) attempts = 0;
    free(text);
    return attempts;
}

static void count(void) {
    size_t i;
    unsigned ready = 0, failed = 0;
    for (i = 0; i < b.set_count; ++i) {
        ready += b.sets[i].state == SET_READY;
        failed += b.sets[i].state == SET_FAILED;
    }
    b.status.ready = ready; b.status.failed = failed;
    if (!b.status.first_tier_seconds && b.set_count) {
        int left = 0;
        for (i = 0; i < b.set_count && !left; ++i)
            if ((priority(&b.sets[i]) >> 40) <= 2 && !b.sets[i].promoted && b.sets[i].state < SET_READY) left = 1;
        if (!left) { b.status.first_tier_seconds = now() - b.start; b.status.first_tier_cpu = cpu_seconds() - b.start_cpu; }
    }
}

/* Called with the guard released: opening a module maps and checks it. */
static int deliver(const char *key, int32_t index, char *error, size_t size) {
    char path[PATH_CAP];
    if (!module_path(path, key, suffix())) { snprintf(error, size, "path too long"); return 0; }
    return b.config.deliver(b.config.deliver_user, path, index, error, size);
}

static void write_failure(set_t *s, const char *key, const char *reason);

/* Hands the module over once and settles every set it serves. Called with the
 * guard held; releases it around the hand-over, during which the sets stay
 * DELIVERING so no worker takes them up again. */
static void serve(const char *key, size_t first) {
    known_t *k = add_known(key);
    char error[256] = "";
    size_t i;
    set_t *s = &b.sets[first];
    s->state = SET_DELIVERING;
    s->job = NULL;
    snprintf(s->key, sizeof(s->key), "%s", key);
    if (!k) { stop_building("out of memory"); return; }
    if (k->state == KNOWN_DELIVERING) return;
    if (k->state == KNOWN_NEW) {
        int ok;
        k->state = KNOWN_DELIVERING;
        unlock(&b.guard);
        ok = deliver(key, s->info.index, error, sizeof(error));
        lock(&b.guard);
        k->state = ok ? KNOWN_DELIVERED : KNOWN_REFUSED;
        if (!ok) {
            char path[PATH_CAP];
            fprintf(stderr, "native: module for %s refused: %s\n", s->info.name, error);
            set_error("module for %s refused: %s", s->info.name, error);
            /* Never loaded, so it can go; a later launch may build it again. */
            if (b.builder && module_path(path, key, ".ok")) remove_file(path);
            if (b.builder && module_path(path, key, suffix())) remove_file(path);
        }
    }
    for (i = 0; i < b.set_count; ++i) {
        set_t *other = &b.sets[i];
        if (other->state != SET_DELIVERING || strcmp(other->key, key)) continue;
        if (k->state == KNOWN_DELIVERED) other->state = SET_READY;
        else {
            if (b.builder) write_failure(other, key, *error ? error : "refused by the loader");
            other->state = SET_FAILED;
        }
    }
    count();
    cond_all(&b.changed);
}

static void write_set_receipt(const set_t *s, const char *key) {
    char path[PATH_CAP], text[80];
    snprintf(text, sizeof(text), "%s\n", key);
    if (!join(path, b.sets_dir, s->info.identity) || !write_atomic(path, text, strlen(text)))
        stop_building("cannot write the native code cache");
    else if (join(path, b.failed_dir, s->info.identity)) remove_file(path);
}

/* Lines that explain a failure, with the build folder removed from paths. The
 * compiler prints no source text: diagnostics run without carets. */
static void keep_tail(job_t *j, const char *log) {
    size_t length = 0;
    char *text = read_small(log, 1u << 16, &length), *line, *next, *out = j->tail;
    size_t left = sizeof(j->tail) - 1, directory = strlen(j->directory);
    if (!text) return;
    if (length > 4096) { memmove(text, text + length - 4096, 4096); text[4096] = 0; }
    for (line = text; line && *line; line = next) {
        char *at;
        size_t n;
        next = strchr(line, '\n');
        if (next) *next++ = 0;
        if (!strstr(line, "error") && !strstr(line, "undefined") && !strstr(line, "lld")) continue;
        while ((at = strstr(line, j->directory))) memmove(at, at + directory + (at[directory] == '/'), strlen(at + directory + (at[directory] == '/')) + 1);
        n = strlen(line);
        if (n + 1 > left) break;
        memcpy(out, line, n); out[n] = '\n'; out += n + 1; left -= n + 1;
    }
    *out = 0;
    free(text);
}

static void write_failure(set_t *s, const char *key, const char *reason) {
    char path[PATH_CAP], *text;
    size_t n = strlen(reason) + 160;
    if (!join(path, b.failed_dir, s->info.identity) || !(text = malloc(n))) return;
    snprintf(text, n, "attempts %u\nmodule %s\n%s", s->attempts + 1, key, reason);
    if (!write_atomic(path, text, strlen(text))) stop_building("cannot write the native code cache");
    free(text);
}

static void finish_failed(job_t *j) {
    size_t i;
    for (i = 0; i < j->set_count; ++i) {
        set_t *s = &b.sets[j->sets[i]];
        write_failure(s, j->key, j->tail);
        s->state = SET_FAILED; s->job = NULL;
        fprintf(stderr, "native: %s failed to compile; it stays interpreted\n", s->info.name);
    }
    count();
}

/* keep: the player is quitting, and the next launch resumes from these files. */
static void forget_job(job_t *j, int keep) {
    job_t **at = &b.jobs;
    size_t i;
    while (*at && *at != j) at = &(*at)->next;
    if (*at) *at = j->next;
    /* A stop keeps what was compiled, whatever ended the job. */
    if (!keep && !b.stopping && b.status.state != NATIVE_BUILD_STOPPED) remove_tree(j->directory);
    for (i = 0; i < j->unit_count; ++i) free(j->units[i]);
    free(j->units); free(j->sets); free(j);
}

static void command_base(const char **argv, size_t *n) {
    size_t i;
    argv[(*n)++] = b.config.compiler;
    for (i = 0; i < b.config.driver_count; ++i) argv[(*n)++] = b.config.driver[i];
    argv[(*n)++] = "-target";
    argv[(*n)++] = b.config.target;
}

static int compile_unit(unsigned worker, job_t *j, size_t unit, int *code) {
    const char *argv[48];
    char object[PATH_CAP], part[PATH_CAP], log[PATH_CAP], define[160], global[PATH_CAP], local[PATH_CAP];
    const char *env[3];
    size_t n = 0;
    snprintf(define, sizeof(define), "-DLH_MODULE_ABI_ID=\"%s\"", b.config.abi_id);
    /* The compiler's caches would keep copies of objects; they live and die with the build. */
    if (!path_of(object, "%s.o", j->units[unit]) || !path_of(part, "%s.o.part", j->units[unit]) || !path_of(log, "%s.log", j->units[unit]) ||
        !path_of(global, "ZIG_GLOBAL_CACHE_DIR=%s/cache", j->directory) || !path_of(local, "ZIG_LOCAL_CACHE_DIR=%s/cache", j->directory))
        return 0;
    /* Finished by an earlier launch: an object exists only once it is whole. */
    if (exists(object, NULL)) { *code = 0; return 1; }
    env[0] = global; env[1] = local; env[2] = NULL;
    command_base(argv, &n);
    argv[n++] = "-mcpu=baseline"; argv[n++] = "-O2"; argv[n++] = "-s"; argv[n++] = "-fomit-frame-pointer";
    argv[n++] = "-std=c99"; argv[n++] = "-DNDEBUG"; argv[n++] = "-fPIC"; argv[n++] = "-w";
    argv[n++] = "-fno-caret-diagnostics"; argv[n++] = "-fno-color-diagnostics";
    if (b.config.windows) argv[n++] = "-mno-stack-arg-probe";
    argv[n++] = "-DFF8_NATIVE_MODULE"; argv[n++] = define;
    argv[n++] = "-I"; argv[n++] = b.include;
    argv[n++] = "-c"; argv[n++] = j->units[unit]; argv[n++] = "-o"; argv[n++] = part;
    argv[n] = NULL;
    if (run(argv, env, log, &b.children[worker], &b.guard, code)) return 0;
    if (*code) keep_tail(j, log);
    else if (!publish(part, object, 1)) *code = 1;
    return 1;
}

static int link_module(unsigned worker, job_t *j, const char *output, int *code) {
    const char **argv = malloc((j->unit_count + 24) * sizeof(*argv));
    char log[PATH_CAP], global[PATH_CAP], local[PATH_CAP], **objects = calloc(j->unit_count, sizeof(*objects));
    const char *env[3];
    size_t n = 0, i;
    int ok = argv && objects && path_of(log, "%s/link.log", j->directory) &&
             path_of(global, "ZIG_GLOBAL_CACHE_DIR=%s/cache", j->directory) && path_of(local, "ZIG_LOCAL_CACHE_DIR=%s/cache", j->directory);
    env[0] = global; env[1] = local; env[2] = NULL;
    if (ok) {
        command_base(argv, &n);
        argv[n++] = "-shared"; argv[n++] = "-nostdlib"; argv[n++] = "-s";
        /* Anything but the call table is a mistake; fail here rather than at load. */
        if (!b.config.windows) argv[n++] = "-Wl,--no-undefined";
        argv[n++] = "-o"; argv[n++] = output;
        for (i = 0; i < j->unit_count && ok; ++i) {
            size_t length = strlen(j->units[i]) + 3;
            if (!(objects[i] = malloc(length))) ok = 0;
            else { snprintf(objects[i], length, "%s.o", j->units[i]); argv[n++] = objects[i]; }
        }
        argv[n] = NULL;
    }
    if (ok) ok = !run(argv, env, log, &b.children[worker], &b.guard, code);
    if (ok && *code) keep_tail(j, log);
    for (i = 0; objects && i < j->unit_count; ++i) free(objects[i]);
    free(objects); free(argv);
    return ok;
}

/* Under the guard: the module now exists; record it for every set it serves.
 * Returns 0 when it could not be published. */
static int finish_built(job_t *j) {
    size_t i;
    char text[96];
    char module[PATH_CAP], receipt[PATH_CAP], built[PATH_CAP];
    uint64_t size = 0;
    if (!path_of(built, "%s/module%s", j->directory, suffix()) || !module_path(module, j->key, suffix()) || !module_path(receipt, j->key, ".ok") ||
        !exists(built, &size) || !sync_file(built) || !publish(built, module, 0)) {
        stop_building("cannot publish a module to the native code cache");
        return 0;
    }
    sync_directory(b.modules);
    snprintf(text, sizeof(text), "lionheart-module 1 size %llu\n", (unsigned long long)size);
    if (!write_atomic(receipt, text, strlen(text))) { stop_building("cannot write the native code cache"); return 0; }
    sync_directory(b.modules);
    for (i = 0; i < j->set_count; ++i) write_set_receipt(&b.sets[j->sets[i]], j->key);
    b.status.compiled++;
    b.compiled_work += j->work;
    say("built %s (%s)", b.sets[j->sets[0]].info.name, j->key);
    return 1;
}

static void add_unit(void *user, const char *name, int directory) {
    job_t *j = user;
    size_t n = strlen(name), length;
    char **grown;
    if (directory || n < 3 || strcmp(name + n - 2, ".c")) return;
    if (!(grown = realloc(j->units, (j->unit_count + 1) * sizeof(*grown)))) { j->failed = 1; return; }
    j->units = grown;
    length = strlen(j->directory) + n + 2;
    if (!(j->units[j->unit_count] = malloc(length))) { j->failed = 1; return; }
    snprintf(j->units[j->unit_count++], length, "%s/%s", j->directory, name);
}

static size_t best_pending(void) {
    size_t i, best = SIZE_MAX;
    uint64_t best_priority = UINT64_MAX;
    for (i = 0; i < b.set_count; ++i) {
        uint64_t p;
        if (b.sets[i].state != SET_PENDING) continue;
        p = priority(&b.sets[i]);
        if (p < best_priority) { best_priority = p; best = i; }
    }
    return best;
}

static job_t *best_job(void) {
    job_t *j, *best = NULL;
    uint64_t best_priority = UINT64_MAX;
    for (j = b.jobs; j; j = j->next) {
        uint64_t p;
        if (j->failed || j->started == j->unit_count) continue;
        p = job_priority(j);
        if (p < best_priority) { best_priority = p; best = j; }
    }
    return best;
}

static int building(void) {
    return b.status.state == NATIVE_BUILD_BUILDING && !b.stopping;
}

/* Under the guard; returns with it held. */
static void emit_set(size_t row) {
    set_t *s = &b.sets[row];
    char directory[PATH_CAP], key[65], error[512] = "";
    int ok;
    job_t *j;
    known_t *k;
    s->state = SET_EMITTING;
    ok = path_of(directory, "%s/%lu-%u", b.work, (unsigned long)
#ifdef _WIN32
                 GetCurrentProcessId(),
#else
                 getpid(),
#endif
                 ++b.sequence);
    unlock(&b.guard);
    ok = ok && make_directory(directory);
    if (ok) {
        lock(&b.emit_guard);
        ok = b.source.emit(b.source.user, row, directory, key, error, sizeof(error));
        unlock(&b.emit_guard);
        if (!ok) remove_tree(directory);
    } else snprintf(error, sizeof(error), "cannot create a build folder");
    lock(&b.guard);
    if (!ok) {
        s->state = SET_PENDING;
        stop_building("cannot generate %s: %s", s->info.name, error);
        return;
    }
    k = find_known(key);
    for (j = b.jobs; j && strcmp(j->key, key); j = j->next) {}
    if (j) {
        /* Another set produced the same code and its module is on the way. */
        size_t *grown = realloc(j->sets, (j->set_count + 1) * sizeof(*grown));
        if (!grown) { s->state = SET_PENDING; stop_building("out of memory"); return; }
        j->sets = grown; j->sets[j->set_count++] = row;
        s->state = SET_COMPILING; s->job = j;
        b.status.shared++;
        unlock(&b.guard);
        remove_tree(directory);
        lock(&b.guard);
        return;
    }
    if ((k && k->state == KNOWN_DELIVERED) || module_valid(key)) {
        b.status.shared++;
        write_set_receipt(s, key);
        serve(key, row);
        unlock(&b.guard);
        remove_tree(directory);
        lock(&b.guard);
        return;
    }
    {
        /* Builds are kept under their key, so a later launch resumes one that
         * a quit or a crash interrupted instead of compiling it all again. */
        char kept[PATH_CAP];
        if (path_of(kept, "%s/%s", b.work, key)) {
            unlock(&b.guard);
            if (exists(kept, NULL)) { remove_tree(directory); snprintf(directory, sizeof(directory), "%s", kept); }
            else if (rename_directory(directory, kept)) snprintf(directory, sizeof(directory), "%s", kept);
            lock(&b.guard);
        }
    }
    if (!(j = calloc(1, sizeof(*j))) || !(j->sets = malloc(sizeof(*j->sets)))) {
        free(j);
        stop_building("out of memory");
        return;
    }
    snprintf(j->key, sizeof(j->key), "%s", key);
    snprintf(j->directory, sizeof(j->directory), "%s", directory);
    j->sets[0] = row; j->set_count = 1;
    j->work = s->info.work ? s->info.work : s->info.size;
    if (!list(directory, add_unit, j) || !j->unit_count) j->failed = 1;
    {
        size_t i;
        char path[PATH_CAP];
        for (i = 0; i < b.config.file_count && !j->failed; ++i) {
            size_t n = strlen(b.config.files[i].name);
            char **grown;
            if (n < 3 || strcmp(b.config.files[i].name + n - 2, ".c") || !join(path, b.include, b.config.files[i].name)) continue;
            if (!(grown = realloc(j->units, (j->unit_count + 1) * sizeof(*grown))) || !(grown[j->unit_count] = malloc(strlen(directory) + n + 2))) {
                if (grown) j->units = grown;
                j->failed = 1; break;
            }
            j->units = grown;
            /* Each module gets its own copy, so every object lands in the build folder. */
            snprintf(j->units[j->unit_count], strlen(directory) + n + 2, "%s/%s", directory, b.config.files[i].name);
            if (!write_file(j->units[j->unit_count], b.config.files[i].data, b.config.files[i].size)) j->failed = 1;
            j->unit_count++;
        }
    }
    if (j->failed) {
        free(j->sets); j->unit_count = 0; free(j->units); remove_tree(directory); free(j);
        s->state = SET_PENDING;
        stop_building("cannot prepare the sources of %s", s->info.name);
        return;
    }
    s->state = SET_COMPILING; s->job = j;
    j->next = b.jobs; b.jobs = j;
    cond_all(&b.changed);
}

/* Under the guard: a unit or link step has ended. */
static void unit_done(unsigned worker, job_t *j) {
    char output[PATH_CAP];
    int code = 0, ran;
    size_t i;
    j->finished++;
    /* A failed module starts no further units; it ends when the running ones do. */
    if (j->finished < j->started || (!j->failed && j->started < j->unit_count)) return;
    if (!j->failed) {
        j->linking = 1;
        unlock(&b.guard);
        ran = path_of(output, "%s/module%s", j->directory, suffix()) && link_module(worker, j, output, &code);
        lock(&b.guard);
        j->linking = 0;
        if (!ran) {
            if (!b.stopping) stop_building("cannot run the compiler: %s", b.config.compiler);
            for (i = 0; i < j->set_count; ++i) { b.sets[j->sets[i]].state = SET_PENDING; b.sets[j->sets[i]].job = NULL; }
            forget_job(j, 0);
            return;
        }
        if (code) j->failed = 1;
        else {
            /* A module published whole is handed over even after a stop elsewhere. */
            if (!finish_built(j)) {
                for (i = 0; i < j->set_count; ++i) { b.sets[j->sets[i]].state = SET_PENDING; b.sets[j->sets[i]].job = NULL; }
                forget_job(j, 0);
                return;
            }
            {
                char key[65];
                size_t first = j->sets[0];
                snprintf(key, sizeof(key), "%s", j->key);
                for (i = 0; i < j->set_count; ++i) {
                    set_t *s = &b.sets[j->sets[i]];
                    s->state = SET_DELIVERING; s->job = NULL;
                    snprintf(s->key, sizeof(s->key), "%s", key);
                }
                forget_job(j, 0);
                serve(key, first);
            }
            return;
        }
    }
    /* Only the module's own failure is held against its sets; a stop is not. */
    if (b.stopping || b.status.state == NATIVE_BUILD_STOPPED) {
        for (i = 0; i < j->set_count; ++i) { b.sets[j->sets[i]].state = SET_PENDING; b.sets[j->sets[i]].job = NULL; }
    } else finish_failed(j);
    forget_job(j, 0);
}

static void lower_priority(void) {
#ifdef _WIN32
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
#else
    /* Linux keeps a nice value per thread; the compilers inherit it. */
    pid_t thread = (pid_t)syscall(SYS_gettid);
    int current = getpriority(PRIO_PROCESS, (id_t)thread);
    setpriority(PRIO_PROCESS, (id_t)thread, current + 10 > 19 ? 19 : current + 10);
#endif
}

static int all_settled(void) {
    size_t i;
    for (i = 0; i < b.set_count; ++i) if (b.sets[i].state != SET_READY && b.sets[i].state != SET_FAILED) return 0;
    return 1;
}

#ifdef _WIN32
static DWORD WINAPI worker_main(LPVOID argument)
#else
static void *worker_main(void *argument)
#endif
{
    unsigned worker = (unsigned)(uintptr_t)argument;
    lower_priority();
    lock(&b.guard);
    for (;;) {
        job_t *j;
        size_t row;
        if (!building()) break;
        if ((j = best_job())) {
            size_t unit = j->started++;
            int code = 0, ran;
            unlock(&b.guard);
            ran = compile_unit(worker, j, unit, &code);
            lock(&b.guard);
            if (!ran) {
                if (!b.stopping) stop_building("cannot run the compiler: %s", b.config.compiler);
                j->failed = 1;
            } else if (code) j->failed = 1;
            unit_done(worker, j);
            continue;
        }
        row = best_pending();
        if (row != SIZE_MAX && b.source_open) { emit_set(row); continue; }
        if (all_settled()) {
            b.status.state = NATIVE_BUILD_DONE;
            b.status.done_seconds = now() - b.start;
            b.status.done_cpu = cpu_seconds() - b.start_cpu;
            cond_all(&b.changed);
            break;
        }
        cond_wait(&b.changed, &b.guard, 0);
    }
    unlock(&b.guard);
#ifdef _WIN32
    return 0;
#else
    return NULL;
#endif
}

static void folder_entry(void *user, const char *name, int directory) {
    char path[PATH_CAP], in_use[PATH_CAP];
    file_lock_t held;
    (void)user;
    if (!directory || !strcmp(name, b.config.cache_id)) return;
    if (!join(path, b.root, name) || !join(in_use, path, "in-use")) return;
    held = file_lock(in_use, 1, 0);
    if (held == NO_FILE_LOCK) return;
    /* Nobody uses it: no player of that version is running. */
    say("removing unused cache %s", name);
    remove_tree(path);
    file_unlock(held);
    remove_tree(path);
}

/* Keeps receipted modules and their receipts. Anything else in the folder is
 * an interrupted publication: a module whose receipt never got written was
 * never loaded by anyone, so it can go. */
static void clean_modules(void *user, const char *name, int directory) {
    char path[PATH_CAP], key[65];
    size_t n = strlen(name), s = strlen(suffix());
    int keep = 0;
    (void)user;
    if (!join(path, b.modules, name)) return;
    if (!directory && ((n == 64 + s && !strcmp(name + 64, suffix())) || (n == 67 && !strcmp(name + 64, ".ok")))) {
        memcpy(key, name, 64); key[64] = 0;
        keep = module_valid(key);
    }
    if (!keep) directory ? remove_tree(path) : remove_file(path);
}

static void clean_part(void *user, const char *name, int directory) {
    const char *parent = user;
    char path[PATH_CAP];
    size_t n = strlen(name);
    if (directory || !join(path, parent, name)) return;
    if (n > 5 && !strcmp(name + n - 5, ".part")) remove_file(path);
}

/* An interrupted build of a module not yet published is kept to resume;
 * anything else in work/ is a leftover. */
static void clean_work(void *user, const char *name, int directory) {
    char path[PATH_CAP];
    (void)user;
    if (!join(path, b.work, name)) return;
    if (directory && is_key(name) && !module_valid(name)) list(path, clean_part, path);
    else if (directory) remove_tree(path);
    else remove_file(path);
}

static void clean_temporary(void *user, const char *name, int directory) {
    const char *parent = user;
    char path[PATH_CAP];
    size_t n = strlen(name);
    if (!directory && n > 4 && !strcmp(name + n - 4, ".tmp") && join(path, parent, name)) remove_file(path);
}

/* Only a builder runs this: nothing it removes can be in use. */
static int become_builder(void) {
    char path[PATH_CAP];
    size_t i;
    if (!join(path, b.cache, "lock")) return 0;
    b.builder_lock = file_lock(path, 1, 0);
    if (b.builder_lock == NO_FILE_LOCK) return 0;
    b.builder = 1;
    list(b.work, clean_work, NULL);
    if (!make_directory(b.work) || !make_directory(b.include) || !make_directory(b.modules) ||
        !make_directory(b.sets_dir) || !make_directory(b.failed_dir)) return -1;
    list(b.modules, clean_modules, NULL);
    list(b.sets_dir, clean_temporary, b.sets_dir);
    list(b.failed_dir, clean_temporary, b.failed_dir);
    list(b.include, clean_temporary, b.include);
    list(b.root, folder_entry, NULL);
    for (i = 0; i < b.config.file_count; ++i) {
        const native_build_file_t *f = &b.config.files[i];
        size_t length = 0;
        char *current;
        if (!join(path, b.include, f->name)) return -1;
        current = read_small(path, f->size + 1, &length);
        if (!current || length != f->size || memcmp(current, f->data, f->size)) {
            if (!write_atomic(path, f->data, f->size)) { free(current); return -1; }
        }
        free(current);
    }
    return 1;
}

/* Hands over every set the cache already has, best first. Under the guard. */
static void load_cached(void) {
    size_t *order = malloc(b.set_count * sizeof(*order)), i, k, n = 0;
    if (!order) return;
    for (i = 0; i < b.set_count; ++i) if (b.sets[i].state == SET_PENDING) order[n++] = i;
    /* Insertion order by priority; a few hundred rows. */
    for (i = 1; i < n; ++i) {
        size_t row = order[i];
        uint64_t p = priority(&b.sets[row]);
        for (k = i; k && priority(&b.sets[order[k - 1]]) > p; --k) order[k] = order[k - 1];
        order[k] = row;
    }
    for (i = 0; i < n && !b.stopping; ++i) {
        set_t *s = &b.sets[order[i]];
        char key[65];
        known_t *k;
        if (s->state != SET_PENDING || !read_receipt(s, key)) continue;
        /* A module the loader refused earlier in this run is not offered again. */
        if ((k = find_known(key)) && k->state == KNOWN_REFUSED) continue;
        if (!module_valid(key)) continue;
        if (b.status.state == NATIVE_BUILD_LOADING) b.status.cached++;
        serve(key, order[i]);
    }
    free(order);
}

static void mark_failures(void) {
    size_t i;
    for (i = 0; i < b.set_count; ++i) {
        set_t *s = &b.sets[i];
        if (s->state != SET_PENDING) continue;
        s->attempts = read_failure(s);
        /* One retry on a later launch; after that the set stays interpreted. */
        if (s->attempts >= 2) s->state = SET_FAILED;
    }
    count();
}

static void drain_samples(void) {
    unsigned write = __atomic_load_n(&b.ring_write, __ATOMIC_ACQUIRE);
    if (write - b.ring_read > RING) b.ring_read = write - RING;
    while (b.ring_read != write) {
        sample_t sample = b.ring[b.ring_read % RING];
        size_t i, matches[SPECIFIC_MATCHES], found = 0;
        b.ring_read++;
        if (!b.source_open || !b.source.contains) continue;
        for (i = 1; i < b.set_count && found <= SPECIFIC_MATCHES; ++i) {
            if (!b.source.contains(b.source.user, i, sample.pc, sample.bytes, sample.size)) continue;
            if (found < SPECIFIC_MATCHES) matches[found] = i;
            found++;
        }
        if (found > SPECIFIC_MATCHES) continue;
        for (i = 0; i < found; ++i) {
            set_t *s = &b.sets[matches[i]];
            if (s->state == SET_READY || s->state == SET_FAILED) continue;
            if (!s->promoted) say("guest needs %s; moving it ahead", s->info.name);
            s->promoted = ++b.promotions;
        }
    }
}

static void estimate(void) {
    double elapsed = now() - b.compile_start;
    uint64_t left = 0;
    size_t i;
    char *out = b.status.compiling;
    size_t room = sizeof(b.status.compiling);
    job_t *j;
    for (i = 0; i < b.set_count; ++i)
        if (b.sets[i].state != SET_READY && b.sets[i].state != SET_FAILED) left += b.sets[i].info.work ? b.sets[i].info.work : b.sets[i].info.size;
    b.status.seconds_left = b.compiled_work && b.compile_start > 0 ? (int)(elapsed * (double)left / (double)b.compiled_work + 0.5) : -1;
    if (!left) b.status.seconds_left = 0;
    *out = 0;
    for (j = b.jobs; j; j = j->next) {
        const char *name = b.sets[j->sets[0]].info.name;
        job_t *seen;
        int n;
        for (seen = b.jobs; seen != j && strcmp(b.sets[seen->sets[0]].info.name, name); seen = seen->next) {}
        if (seen != j) continue;
        n = snprintf(out, room, "%s%s", out == b.status.compiling ? "" : ",", name);
        if (n < 0 || (size_t)n >= room) { *out = 0; break; }
        out += n; room -= (size_t)n;
    }
}

static unsigned pick_workers(void) {
    unsigned cpus = processors(), by_memory, workers;
    uint64_t memory = available_memory();
    if (b.config.workers) return b.config.workers;
    /* Leave the game its own core, and its sibling where cores are shared. */
    workers = cpus >= 4 ? cpus - 2 : 1;
    by_memory = memory > (256ull << 20) ? (unsigned)((memory - (256ull << 20)) / COMPILER_BYTES) : 1;
    if (!by_memory) by_memory = 1;
    return workers < by_memory ? workers : by_memory;
}

static int start_workers(void) {
    unsigned i;
    b.worker_count = pick_workers();
    b.status.workers = b.worker_count;
    b.workers = calloc(b.worker_count, sizeof(*b.workers));
    b.children = calloc(b.worker_count, sizeof(*b.children));
    if (!b.workers || !b.children) return 0;
    b.compile_start = now();
    for (i = 0; i < b.worker_count; ++i) {
#ifdef _WIN32
        if (!(b.workers[i] = CreateThread(NULL, 0, worker_main, (LPVOID)(uintptr_t)i, 0, NULL))) { b.worker_count = i; return 0; }
#else
        if (pthread_create(&b.workers[i], NULL, worker_main, (void *)(uintptr_t)i)) { b.worker_count = i; return 0; }
#endif
    }
    return 1;
}

static int open_source(void) {
    char error[512] = "";
    native_build_set_t *rows = NULL;
    size_t count = 0, i;
    int ok;
    b.status.state = NATIVE_BUILD_LOADING;
    unlock(&b.guard);
    ok = b.source.open(b.source.user, error, sizeof(error));
    if (ok && b.source.plan(b.source.user, b.config.discs, &rows, &count, error, sizeof(error))) {
        /* The discs must give exactly the sets the metadata promised. */
        ok = count == b.set_count;
        for (i = 0; ok && i < count; ++i)
            ok = rows[i].index == b.sets[i].info.index && !strcmp(rows[i].identity, b.sets[i].info.identity);
        if (!ok) snprintf(error, sizeof(error), "the discs do not match the catalog plan");
        for (i = 0; ok && i < count; ++i) b.sets[i].info = rows[i];
    }
    free(rows);
    lock(&b.guard);
    if (!ok) { stop_building("%s", *error ? error : "cannot read the discs"); return 0; }
    b.source_open = 1;
    b.status.state = NATIVE_BUILD_BUILDING;
    return 1;
}

#ifdef _WIN32
static DWORD WINAPI service_main(LPVOID argument)
#else
static void *service_main(void *argument)
#endif
{
    native_build_set_t *rows = NULL;
    size_t count = 0, i;
    char error[512] = "", path[PATH_CAP], in_use[PATH_CAP];
    int role;
    double last_report = 0;
    (void)argument;
    if (!b.source.plan(b.source.user, b.config.discs, &rows, &count, error, sizeof(error)) || !count) {
        lock(&b.guard);
        stop_building("cannot list the sets: %s", error);
        unlock(&b.guard);
        free(rows);
        goto finished;
    }
    lock(&b.guard);
    b.sets = calloc(count, sizeof(*b.sets));
    if (!b.sets) { stop_building("out of memory"); unlock(&b.guard); free(rows); goto finished; }
    for (i = 0; i < count; ++i) b.sets[i].info = rows[i];
    b.set_count = count;
    b.status.sets = (unsigned)count;
    free(rows);
    if (!make_directory(b.root) || !join(path, b.root, b.config.cache_id) || !make_directory(path) ||
        !make_directory(b.cache) || !join(in_use, path, "in-use") ||
        (b.in_use = file_lock(in_use, 0, 1)) == NO_FILE_LOCK) {
        stop_building("cannot use the native code cache");
        unlock(&b.guard);
        goto finished;
    }
    role = become_builder();
    if (role < 0) { stop_building("cannot write the native code cache"); unlock(&b.guard); goto finished; }
    load_cached();
    b.status.cached_seconds = now() - b.start;
    say("%u of %u sets from the cache in %.3f s", b.status.ready, b.status.sets, b.status.cached_seconds);
    mark_failures();
    while (!b.stopping && b.status.state != NATIVE_BUILD_STOPPED) {
        if (all_settled() && !b.jobs) {
            if (b.status.state != NATIVE_BUILD_DONE) {
                b.status.state = NATIVE_BUILD_DONE;
                b.status.done_seconds = now() - b.start;
                b.status.done_cpu = cpu_seconds() - b.start_cpu;
                say("done: %u sets native, %u failed, %u modules compiled, %u sets shared a module, %.1f s",
                    b.status.ready, b.status.failed, b.status.compiled, b.status.shared, b.status.done_seconds);
                cond_all(&b.changed);
            }
            break;
        }
        if (!b.builder) {
            /* Another player builds; take what it publishes, and take over if it leaves. */
            b.status.state = NATIVE_BUILD_WAITING;
            role = become_builder();
            if (role < 0) { stop_building("cannot write the native code cache"); break; }
            load_cached();
            if (!b.builder) { cond_wait(&b.changed, &b.guard, 2000); continue; }
            mark_failures();
            continue;
        }
        if (!b.source_open) {
            if (!open_source()) break;
            if (!start_workers()) { stop_building("cannot start build threads"); break; }
            say("building %u sets with %u workers", b.status.sets - b.status.ready - b.status.failed, b.worker_count);
        }
        drain_samples();
        estimate();
        if (b.config.verbose && now() - last_report >= 10) {
            last_report = now();
            say("%u/%u ready, %u failed, compiling %s, about %d s left", b.status.ready, b.status.sets, b.status.failed,
                *b.status.compiling ? b.status.compiling : "-", b.status.seconds_left);
        }
        cond_wait(&b.changed, &b.guard, 100);
    }
    estimate();
    if (b.status.state == NATIVE_BUILD_DONE && b.builder) {
        /* Done: leftovers of interrupted builds whose sets went away. */
        unlock(&b.guard);
        remove_tree(b.work);
        make_directory(b.work);
        lock(&b.guard);
    }
    if (b.status.state == NATIVE_BUILD_DONE && b.source_open) {
        /* Nothing is left to emit: the discs' images need not stay in memory
         * for the rest of the game. No emit can be running once all is settled. */
        b.source_open = 0;
        unlock(&b.guard);
        lock(&b.emit_guard);
        if (b.source.close) b.source.close(b.source.user);
        unlock(&b.emit_guard);
        lock(&b.guard);
    }
    unlock(&b.guard);
finished:
    lock(&b.guard);
    cond_all(&b.changed);
    unlock(&b.guard);
#ifdef _WIN32
    return 0;
#else
    return NULL;
#endif
}

int native_build_start(const native_build_config_t *config, char *error, size_t size) {
    size_t i;
    if (b.running) { snprintf(error, size, "already running"); return 0; }
    if (!config->root || !config->cache_id || !config->exe_id || !config->compiler || !config->target || !config->abi_id ||
        !config->source || !config->source->plan || !config->source->open || !config->source->emit || !config->deliver) {
        snprintf(error, size, "incomplete configuration"); return 0;
    }
    memset(&b, 0, sizeof(b));
    b.config = *config;
    b.source = *config->source;
    b.in_use = b.builder_lock = NO_FILE_LOCK;
    b.disc = config->disc;
    snprintf(b.root, sizeof(b.root), "%s", config->root);
    if (snprintf(b.cache, sizeof(b.cache), "%s/%s/%s", config->root, config->cache_id, config->exe_id) >= (int)sizeof(b.cache) - 80 ||
        !join(b.include, b.cache, "include") || !join(b.modules, b.cache, "modules") || !join(b.sets_dir, b.cache, "sets") ||
        !join(b.failed_dir, b.cache, "failed") || !join(b.work, b.cache, "work")) {
        snprintf(error, size, "data path too long"); return 0;
    }
    for (i = 0; i < config->file_count; ++i)
        if (strchr(config->files[i].name, '/') || strchr(config->files[i].name, '\\')) { snprintf(error, size, "bad support file name"); return 0; }
    lock_init(&b.guard); lock_init(&b.emit_guard); cond_init(&b.changed);
    b.status.state = NATIVE_BUILD_LOADING;
    b.status.seconds_left = -1;
    b.start = now();
    b.start_cpu = cpu_seconds();
#ifdef _WIN32
    if (!native_build_job && (native_build_job = CreateJobObjectW(NULL, NULL))) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits;
        memset(&limits, 0, sizeof(limits));
        /* Compilers end with the player, however it ends. */
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        SetInformationJobObject(native_build_job, JobObjectExtendedLimitInformation, &limits, sizeof(limits));
    }
    if (!(b.service = CreateThread(NULL, 0, service_main, NULL, 0, NULL))) { snprintf(error, size, "cannot start a thread"); return 0; }
#else
    reaper_start();
    if (pthread_create(&b.service, NULL, service_main, NULL)) { reaper_stop(); snprintf(error, size, "cannot start a thread"); return 0; }
#endif
    b.running = 1;
    return 1;
}

void native_build_stop(void) {
    unsigned i;
    if (!b.running) return;
    lock(&b.guard);
    b.stopping = 1;
    for (i = 0; i < b.worker_count; ++i) {
        child_t *c = &b.children[i];
        /* Marked so a worker between runs starts no new compiler. */
#ifdef _WIN32
        if (c->active > 0 && c->process) TerminateProcess(c->process, 1);
#else
        if (c->active > 0) kill(-c->pid, SIGKILL);
#endif
        c->active = -1;
    }
    cond_all(&b.changed);
    unlock(&b.guard);
#ifdef _WIN32
    WaitForSingleObject(b.service, INFINITE); CloseHandle(b.service);
    for (i = 0; i < b.worker_count; ++i) { WaitForSingleObject(b.workers[i], INFINITE); CloseHandle(b.workers[i]); }
#else
    pthread_join(b.service, NULL);
    for (i = 0; i < b.worker_count; ++i) pthread_join(b.workers[i], NULL);
    reaper_stop();
#endif
    while (b.jobs) forget_job(b.jobs, 1);
    if (b.source_open && b.source.close) b.source.close(b.source.user);
    b.source_open = 0;
    file_unlock(b.builder_lock);
    file_unlock(b.in_use);
    free(b.workers); free(b.children); free(b.sets); free(b.known);
    b.workers = NULL; b.children = NULL; b.sets = NULL; b.known = NULL;
    b.running = 0;
}

void native_build_status(native_build_status_t *status) {
    if (!b.running && b.status.state == NATIVE_BUILD_OFF) { memset(status, 0, sizeof(*status)); status->seconds_left = -1; return; }
    lock(&b.guard);
    *status = b.status;
    unlock(&b.guard);
}

void native_build_disc(unsigned disc) {
    if (!b.running) return;
    lock(&b.guard);
    b.disc = disc;
    cond_all(&b.changed);
    unlock(&b.guard);
}

void native_build_ran(uint32_t pc, const uint8_t *bytes, size_t size) {
    unsigned at;
    sample_t *s;
    if (!b.running || !b.source_open) return;
    /* One producer, the guest thread; a lagging reader just loses old samples. */
    at = __atomic_load_n(&b.ring_write, __ATOMIC_RELAXED);
    s = &b.ring[at % RING];
    s->pc = pc;
    s->size = (uint8_t)(size > sizeof(s->bytes) ? sizeof(s->bytes) : size);
    memcpy(s->bytes, bytes, s->size);
    __atomic_store_n(&b.ring_write, at + 1, __ATOMIC_RELEASE);
}

native_build_state_t native_build_wait(double seconds) {
    double until = seconds < 0 ? 0 : now() + seconds;
    native_build_state_t state;
    if (!b.running) return b.status.state;
    lock(&b.guard);
    while (b.status.state != NATIVE_BUILD_DONE && b.status.state != NATIVE_BUILD_STOPPED && (!until || now() < until))
        cond_wait(&b.changed, &b.guard, 200);
    state = b.status.state;
    unlock(&b.guard);
    return state;
}
