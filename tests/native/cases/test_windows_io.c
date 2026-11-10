#include "windows_io.h"
#include "player_paths.h"
#include <errno.h>
/* 1 for a directory, 2 for a regular file, 0 when the path is absent, -1 otherwise. */
int lh_stat_kind(const char *path, long long *size) {
    struct _stat64 st;
    if (host_win_stat(path,&st)) return errno == ENOENT ? 0 : -1;
    *size = st.st_size;
    return (st.st_mode & _S_IFMT) == _S_IFDIR ? 1 : (st.st_mode & _S_IFMT) == _S_IFREG ? 2 : -1;
}
int lh_make_directory(const char *path) { return host_win_mkdir(path); }
/* The player's own way of making its data folders; the reason on failure. */
const char *lh_make_tree(const char *path) { return host_player_directory(path) ? "" : host_player_directory_failure(); }
/* Where the player would keep its data on this machine, or "" when it cannot tell. */
const char *lh_data_folder(void) {
    static char path[4096];
    return host_player_data(path,sizeof(path)) ? path : "";
}
