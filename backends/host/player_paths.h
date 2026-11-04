#ifndef FF8_HOST_PLAYER_PATHS_H
#define FF8_HOST_PLAYER_PATHS_H
#include <stddef.h>
/* A portable marker selects the executable directory for both config and data. */
int host_player_portable(char *base, size_t size);
int host_player_data(char *path, size_t size);
int host_player_directory(const char *path);
#endif
