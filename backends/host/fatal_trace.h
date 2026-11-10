#ifndef HOST_FATAL_TRACE_H
#define HOST_FATAL_TRACE_H

#include <signal.h>
#include <unistd.h>
#ifdef _WIN32
#include <windows.h>
#include <io.h>
#endif

static volatile sig_atomic_t fatal_log_fd = 2;
#ifdef _WIN32
static HANDLE fatal_log_handle;
#endif

/* The stderr tee may die before draining its pipe. Bypass it, stdio and SDL. */
static void fatal_line(const char *prefix, unsigned code) {
    char line[64];
    unsigned n = 0, shift;
    while (*prefix) line[n++] = *prefix++;
    for (shift = 32; shift; shift -= 4)
        line[n++] = "0123456789abcdef"[(code >> (shift - 4)) & 15];
    line[n++] = '\n';
#ifdef _WIN32
    DWORD written;
    WriteFile(fatal_log_handle, line, n, &written, NULL);
#else
    (void)write(fatal_log_fd, line, n);
#endif
}

static void fatal_signal(int number) {
    fatal_line("Fatal signal 0x", (unsigned)number);
#ifdef _WIN32
    signal(number, SIG_DFL);
    raise(number);
#else
    /* SA_RESETHAND restores the disposition before entry. The pending signal
     * takes its default action when this handler returns and unblocks it. */
    (void)kill(getpid(), number);
#endif
}

#ifdef _WIN32
static LONG WINAPI fatal_exception(EXCEPTION_POINTERS *exception) {
    fatal_line("Fatal exception 0x", exception->ExceptionRecord->ExceptionCode);
    return EXCEPTION_CONTINUE_SEARCH;
}
#endif

static void fatal_trace_install(int fd) {
    static const int signals[] = {SIGSEGV, SIGABRT, SIGFPE, SIGILL,
#ifdef SIGBUS
        SIGBUS,
#endif
    };
    unsigned i;
    fatal_log_fd = fd;
#ifdef _WIN32
    fatal_log_handle = (HANDLE)_get_osfhandle(fd);
    SetUnhandledExceptionFilter(fatal_exception);
    for (i = 0; i < sizeof(signals)/sizeof(signals[0]); ++i)
        signal(signals[i], fatal_signal);
#else
    struct sigaction action = {0};
    action.sa_handler = fatal_signal;
    action.sa_flags = SA_RESETHAND;
    sigemptyset(&action.sa_mask);
    for (i = 0; i < sizeof(signals)/sizeof(signals[0]); ++i)
        sigaction(signals[i], &action, NULL);
#endif
}
#endif
