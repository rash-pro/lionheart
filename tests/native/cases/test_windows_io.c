#include "windows_io.h"
#include <errno.h>
/* 1 for a directory, 2 for a regular file, 0 when the path is absent, -1 otherwise. */
int lh_stat_kind(const char *path, long long *size) {
    struct _stat64 st;
    if (host_win_stat(path,&st)) return errno == ENOENT ? 0 : -1;
    *size = st.st_size;
    return (st.st_mode & _S_IFMT) == _S_IFDIR ? 1 : (st.st_mode & _S_IFMT) == _S_IFREG ? 2 : -1;
}
int lh_make_directory(const char *path) { return host_win_mkdir(path); }
