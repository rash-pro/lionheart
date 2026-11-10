#ifndef FF8_HOST_PLAYER_PATHS_H
#define FF8_HOST_PLAYER_PATHS_H
#include <stddef.h>
/* A portable marker selects the executable directory for both config and data. */
int host_player_portable(char *base, size_t size);
int host_player_data(char *path, size_t size);
int host_player_directory(const char *path);
/* Why the last host_player_directory call failed: the step, the folder and the system's reason. */
const char *host_player_directory_failure(void);
/* The log of the previous session when that session ended without saying so:
 * a crash, a kill or an abnormal stop. NULL otherwise, and outside the
 * player's session wrapper. */
const char *host_session_crash_log(void);
#endif
