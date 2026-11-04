// Real ImGui navigation must stay in its pane and open the pad keyboard on device switch.
#include "gui_internal.h"
#include "imgui.h"
#include <cassert>
#include <cstdio>
#include <cstring>
extern "C" const char *host_gui_string(const char *id) { return !strcmp(id,"val.on") ? "On" : "Off"; }
static host_gui_input_t source=HOST_GUI_KEYBOARD;
static int edit_focuses;
extern "C" host_gui_input_t host_gui_last_input(void) { return source; }
extern "C" void host_gui_edit_focus(char *,size_t,int) { ++edit_focuses; }
extern "C" void host_gui_test_window(SDL_Window *) {}
static char username[128];
static const char *const renderers[]={"Soft","Vulkan"},*const filters[]={"Nearest","Linear"};
static void frame(SDL_Renderer *renderer,bool edit=false) {
    SDL_Event e; while(SDL_PollEvent(&e)) gui_ui_event(&e);
    gui_ui_begin(1280,720,0);
    gui_ui_region_begin("rail",{0,0,300,664});
    gui_ui_row("nav.play","Play",nullptr,{0,142,300,44},0);
    gui_ui_row("nav.discs","Discs",nullptr,{0,188,300,44},0);
    gui_ui_region_end();
    gui_ui_region_begin("pane",{300,0,980,664});
    if(edit) gui_ui_edit("username",{348,226,380,48},username,sizeof(username),0);
    else {
        gui_ui_control_row({348,116,836,52}); gui_ui_segments("renderer",{1008,120,176,46},renderers,2,0,1);
        gui_ui_control_row({348,172,836,52}); gui_ui_stepper("scale",{988,176,196,46},1,1,8,0);
        gui_ui_control_row({348,340,836,52}); gui_ui_segments("filter",{1008,344,176,46},filters,2,0,1);
    }
    gui_ui_region_end(); gui_ui_end(); gui_ui_soft_render(renderer);
}
int main() {
    assert(SDL_Init(SDL_INIT_VIDEO|SDL_INIT_GAMECONTROLLER)==0);
    SDL_Window *window=SDL_CreateWindow("nav-entry",0,0,1280,720,SDL_WINDOW_HIDDEN); assert(window);
    SDL_Renderer *renderer=SDL_CreateRenderer(window,-1,SDL_RENDERER_SOFTWARE); assert(renderer);
    assert(gui_ui_attach(window,renderer)); gui_ui_focus("renderer");frame(renderer);frame(renderer);
    assert(!strcmp(gui_ui_focused(),"renderer"));
    SDL_Event e={};e.type=SDL_KEYDOWN;e.key.windowID=SDL_GetWindowID(window);e.key.keysym.scancode=SDL_SCANCODE_DOWN;e.key.keysym.sym=SDLK_DOWN;
    assert(SDL_PushEvent(&e)==1);frame(renderer);frame(renderer);
    printf("down_from_renderer=%s\n",gui_ui_focused()); assert(!strcmp(gui_ui_focused(),"filter"));
    e.type=SDL_KEYUP;assert(SDL_PushEvent(&e)==1);frame(renderer);
    gui_ui_reset_input();gui_ui_focus("username");frame(renderer,true);frame(renderer,true);
    int before=edit_focuses; assert(before>0);
    source=HOST_GUI_PAD;frame(renderer,true);
    printf("same_field_pad_callbacks=%d\n",edit_focuses-before);assert(edit_focuses==before+1);
    frame(renderer,true); assert(edit_focuses==before+1);
    gui_ui_detach();SDL_DestroyRenderer(renderer);SDL_DestroyWindow(window);SDL_Quit();
}
