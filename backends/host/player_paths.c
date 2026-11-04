#define _POSIX_C_SOURCE 200809L
#include "player_paths.h"
#include <SDL.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

int host_player_portable(char *base, size_t size) {
    char *sdl = SDL_GetBasePath();
    char marker[4096]; struct stat st; int n;
    if (!sdl) return 0;
    n = snprintf(base,size,"%s",sdl); SDL_free(sdl);
    if (n < 0 || (size_t)n >= size) return 0;
#ifdef _WIN32
    for (char *p=base;*p;++p) if (*p=='\\') *p='/';
#endif
    n = snprintf(marker,sizeof(marker),"%sportable",base);
    return n >= 0 && (size_t)n < sizeof(marker) && !stat(marker,&st) && S_ISREG(st.st_mode);
}
int host_player_data(char *path, size_t size) {
    const char *data, *home; int n;
    if (host_player_portable(path,size)) return 1;
#ifdef _WIN32
    data = getenv("APPDATA"); home = NULL;
    if (!data || !*data) return 0;
    n = snprintf(path,size,"%s/Lionheart",data);
#else
    data = getenv("XDG_DATA_HOME"); home = getenv("HOME");
    if (data && *data) n = snprintf(path,size,"%s/lionheart",data);
    else if (home && *home) n = snprintf(path,size,"%s/.local/share/lionheart",home);
    else return 0;
#endif
    (void)home;
    return n >= 0 && (size_t)n < size;
}
int host_player_directory(const char *path) {
    char *copy = strdup(path), *p; struct stat st; int ok;
    if (!copy || !*copy) { free(copy); return 0; }
    ok = 1;
    for (p = copy + 1; ; ++p) if (*p == '/' || !*p) {
        char saved = *p; *p = 0;
        if ((mkdir(copy,0700) && errno != EEXIST) || stat(copy,&st) || !S_ISDIR(st.st_mode)) ok = 0;
        *p = saved; if (!saved || !ok) break;
    }
    free(copy); return ok;
}
