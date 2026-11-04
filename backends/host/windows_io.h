#ifndef HOST_WINDOWS_IO_H
#define HOST_WINDOWS_IO_H
#ifdef _WIN32
/* Forced into Windows host objects only; shared runtime source stays portable. */
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <io.h>
#include <direct.h>
#include <time.h>
#include <wchar.h>
#ifdef __cplusplus
extern "C" {
#endif
FILE *host_win_fopen(const char *, const char *);
int host_win_open(const char *, int, ...);
int host_win_stat(const char *, struct _stat64 *);
int host_win_mkdir(const char *);
int host_win_unlink(const char *);
int host_win_rename(const char *, const char *);
char *host_win_getenv(const char *);
#ifdef __cplusplus
}
#endif
static inline struct tm *host_win_localtime(const time_t *t, struct tm *out) { return localtime_s(out,t) ? NULL : out; }
#define localtime_r host_win_localtime
#ifndef HOST_WINDOWS_IO_IMPLEMENTATION
#define fopen host_win_fopen
#define open host_win_open
#define _stat64(path, out) host_win_stat(path, out)
#define mkdir(path, mode) host_win_mkdir(path)
#define unlink host_win_unlink
#define remove host_win_unlink
#define rename host_win_rename
#define getenv host_win_getenv
#endif
#endif
#endif
