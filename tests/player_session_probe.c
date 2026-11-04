/* Stub the guest and the dialog only. The real player session/entry/path code
 * runs under Wine, including its non-headless fatal-error branch, without UI. */
#include <SDL.h>
#include <stdio.h>
int host_player_main(int argc, char **argv) {
    (void)argc; (void)argv;
    fputs("fatal startup: supply disc 1\n",stderr);
    for (int i=0;i<10000;++i) fputc('x',stderr);
    return 2;
}
int __wrap_SDL_ShowSimpleMessageBox(Uint32 flags, const char *title, const char *text, SDL_Window *window) {
    (void)flags; (void)window;
    printf("messagebox title=%s text=%.27s\n",title,text);
    return 0;
}
