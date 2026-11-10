#pragma once
#include <stdio.h>
#include <stdlib.h>
#ifndef _WIN32
#include <signal.h>
#include <sys/resource.h>
#endif
static void lh_test_limit_file_size(void) {
#ifdef _WIN32
    puts("SKIP: requires POSIX file size resource limits");
    exit(77);
#else
    struct rlimit limit = {0, 0};
    signal(SIGXFSZ, SIG_IGN);
    if (setrlimit(RLIMIT_FSIZE, &limit)) abort();
#endif
}
