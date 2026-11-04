// Official SDL/ImGui backend: a held controller button must not activate a reopened menu.
#include "gui_internal.h"
#include "imgui.h"
#include "backends/imgui_impl_sdl2.h"
#include <cassert>
#include <cstdio>
#include <cstring>
extern "C" const char *host_gui_string(const char *id) { return !strcmp(id,"val.on") ? "On" : "Off"; }
extern "C" host_gui_input_t host_gui_last_input(void) { return HOST_GUI_PAD; }
extern "C" void host_gui_edit_focus(char *,size_t,int) {}
extern "C" void host_gui_test_window(SDL_Window *) {}
static int actions;
static void frame(SDL_Renderer *renderer) {
    SDL_Event e; while(SDL_PollEvent(&e)) gui_ui_event(&e);
    gui_ui_begin(1280,720,0);
    if(gui_ui_button("resume","Resume",{340,120,250,50},GUI_PRIMARY)) ++actions;
    gui_ui_end(); gui_ui_soft_render(renderer);
}
int main() {
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS,"1");
    assert(SDL_Init(SDL_INIT_VIDEO|SDL_INIT_GAMECONTROLLER)==0);
    SDL_Window *window=SDL_CreateWindow("held-entry",0,0,1280,720,SDL_WINDOW_HIDDEN); assert(window);
    SDL_Renderer *renderer=SDL_CreateRenderer(window,-1,SDL_RENDERER_SOFTWARE); assert(renderer);
    int index=SDL_JoystickAttachVirtual(SDL_JOYSTICK_TYPE_GAMECONTROLLER,6,16,0); assert(index>=0);
    SDL_GameController *pad=SDL_GameControllerOpen(index); assert(pad);
    SDL_Joystick *joy=SDL_GameControllerGetJoystick(pad);
    SDL_JoystickSetVirtualAxis(joy,SDL_CONTROLLER_AXIS_TRIGGERLEFT,-32768);
    SDL_JoystickSetVirtualAxis(joy,SDL_CONTROLLER_AXIS_TRIGGERRIGHT,-32768);
    assert(gui_ui_attach(window,renderer));
    ImGui_ImplSDL2_SetGamepadMode(ImGui_ImplSDL2_GamepadMode_Manual,&pad,1);
    gui_ui_focus("resume"); frame(renderer); frame(renderer);
    assert(actions==0);
    assert(SDL_JoystickSetVirtualButton(joy,SDL_CONTROLLER_BUTTON_A,1)==0); SDL_JoystickUpdate(); SDL_PumpEvents();
    assert(SDL_GameControllerGetButton(pad,SDL_CONTROLLER_BUTTON_A));
    gui_ui_reset_input();
    frame(renderer); frame(renderer); frame(renderer);
    printf("actions_from_button_held_on_entry=%d\n",actions);
    assert(actions==0);
    SDL_JoystickSetVirtualButton(joy,SDL_CONTROLLER_BUTTON_A,0); SDL_JoystickUpdate();
    frame(renderer); frame(renderer); assert(actions==0);
    ImGui_ImplSDL2_SetGamepadMode(ImGui_ImplSDL2_GamepadMode_Manual,&pad,1);
    SDL_JoystickSetVirtualButton(joy,SDL_CONTROLLER_BUTTON_A,1); SDL_JoystickUpdate();
    frame(renderer); frame(renderer);
    printf("actions_after_release_and_fresh_press=%d\n",actions);
    assert(actions==1);
    gui_ui_detach(); SDL_GameControllerClose(pad); SDL_JoystickDetachVirtual(index);
    SDL_DestroyRenderer(renderer); SDL_DestroyWindow(window); SDL_Quit();
    return 0;
}
