/* AppRun: starts the player inside the AppImage. The runtime describes the
 * mount in APPDIR, APPIMAGE, ARGV0 and OWD; the player needs none of them, and
 * the programs it starts, a compiler among them, must see the environment the
 * user launched it with. So those four go and nothing else changes, which is
 * why this is not a shell script: a shell exports variables of its own. The
 * player finds its libraries through its run path, not through this. */
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int main(int argc, char **argv) {
    static const char player[] = "usr/bin/lionheart";
    char path[PATH_MAX];
    ssize_t n = readlink("/proc/self/exe", path, sizeof(path) - sizeof(player));
    char *slash;
    (void)argc;
    if (n <= 0) { perror("AppRun: /proc/self/exe"); return 127; }
    path[n] = 0;
    if (!(slash = strrchr(path, '/'))) return 127;
    memcpy(slash + 1, player, sizeof(player));
    unsetenv("APPDIR"); unsetenv("APPIMAGE"); unsetenv("ARGV0"); unsetenv("OWD");
    argv[0] = path;
    execv(path, argv);
    perror(path);
    return 127;
}
