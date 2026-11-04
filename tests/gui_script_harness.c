/* The driver must queue SDL events rather than invoke model transitions. */
#include "gui_internal.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static int visible;
int host_gui_visible(void) { return visible; }
const char *gui_ui_focused(void) { return "btn.browse"; }
const char *gui_ui_text_for(const char *id) { return !strcmp(id,"app.name") ? "Lionheart" : NULL; }
int main(int argc,char **argv) {
    SDL_Event event; unsigned keys=0,ups=0,text=0,drops=0,tick;
    assert(argc==2 && SDL_Init(SDL_INIT_VIDEO|SDL_INIT_EVENTS)==0);
    SDL_Window *window=SDL_CreateWindow("script",0,0,16,16,0); assert(window);
    assert(host_gui_test_load(argv[1]));
    for (tick=0; tick<1000 && !host_gui_test_failed() && !host_gui_test_finished();++tick) {
        host_gui_test_tick(tick);
        while (SDL_PollEvent(&event)) {
            host_gui_test_event(&event);
            if (event.type==SDL_KEYDOWN) { ++keys; if (event.key.keysym.scancode==SDL_SCANCODE_ESCAPE) visible=!visible; }
            if (event.type==SDL_KEYUP) ++ups;
            if (event.type==SDL_TEXTINPUT) text+=(unsigned)strlen(event.text.text);
            if (event.type==SDL_DROPFILE) { ++drops; assert(event.drop.file && *event.drop.file); SDL_free(event.drop.file); }
        }
        host_gui_test_observe();
        if (host_gui_test_capture()) { assert(visible); host_gui_test_captured(1); }
    }
    printf("keys=%u ups=%u text=%u drops=%u ticks=%u finished=%d failed=%d visible=%d\n",keys,ups,text,drops,tick,host_gui_test_finished(),host_gui_test_failed(),visible);
    int failed=host_gui_test_failed() || !host_gui_test_finished();
    host_gui_test_shutdown(); SDL_DestroyWindow(window); SDL_Quit(); return failed ? 2 : 0;
}
