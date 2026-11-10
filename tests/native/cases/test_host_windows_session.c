
#include <SDL.h>
#include <stdio.h>
#include <string.h>
#include <signal.h>
#include <stdlib.h>
int host_player_main(int argc, char **argv) {
    if (argc == 3 && !strcmp(argv[1], "--crash")) raise(atoi(argv[2]));
    if (argc == 2 && !strcmp(argv[1], "--abnormal")) {
        fputs("Stop: test fault\nLast PC: 80010000\n", stderr);
        return 1;
    }
    fputs("fatal startup: supply disc 1\n",stderr);
    for (int i=0;i<10000;++i) fputc('x',stderr);
    return 2;
}
int __wrap_SDL_ShowSimpleMessageBox(Uint32 flags, const char *title, const char *text, SDL_Window *window) {
    (void)flags; (void)window;
    printf("messagebox title=%s text=%.27s\n",title,text);
    return 0;
}
