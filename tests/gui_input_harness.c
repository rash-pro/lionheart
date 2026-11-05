/* Real SDL controller events and SIO replies across the display's modal seam. */
#include "display.h"
#include "gui_internal.h"
#include "viewport.h"
#include "input.h"
#include "screenshot.h"
#include "hw.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static cpu_t *cpu;
static gpu_t *gpu;
static host_input_t input;
static SDL_JoystickID controller;
static unsigned port,opens,paused_frames,pauses,resumes;
static int visible,requested,result;
static host_gui_pane_t requested_pane;
static uint64_t paused_frame;
static void pause_audio(void) { ++pauses; }
static void resume_audio(void) { ++resumes; }
static void queue_button(unsigned button,int down) {
    SDL_Event event; SDL_zero(event);
    event.type=down ? SDL_CONTROLLERBUTTONDOWN : SDL_CONTROLLERBUTTONUP;
    event.cbutton.which=controller; event.cbutton.button=(Uint8)button;
    assert(SDL_PushEvent(&event)==1);
}
static void queue_axis(unsigned axis,int value) {
    SDL_Event event; SDL_zero(event); event.type=SDL_CONTROLLERAXISMOTION;
    event.caxis.which=controller; event.caxis.axis=(Uint8)axis; event.caxis.value=(Sint16)value;
    assert(SDL_PushEvent(&event)==1);
}
static unsigned wire(unsigned value) {
    hw_write(cpu,0x1f801040,1,value); cpu->cycles+=2000; hw_event(cpu);
    return hw_read(cpu,0x1f801040,1);
}
static void reply(uint16_t expected) {
    unsigned low,high;
    hw_write(cpu,0x1f80104a,2,0); hw_write(cpu,0x1f801048,2,0xd); hw_write(cpu,0x1f80104e,2,136);
    hw_write(cpu,0x1f80104a,2,3|(port<<13));
    assert(wire(1)==255 && wire(0x42)==0x41 && wire(0)==0x5a);
    low=wire(0); high=wire(0); assert(((low|(high<<8))^65535u)==expected);
}
static void pump(void) { host_present(gpu); host_input_sync(cpu,&input); }
static void button(unsigned id,int down) { queue_button(id,down); pump(); }

/* These fakes isolate display-owned event consumption. The actual model,
 * ImGui widgets and screen navigation are exercised by --gui-test runs. */
int host_gui_enabled(void) { return 1; }
int host_gui_visible(void) { return visible; }
int host_gui_toast_visible(void) { return 0; }
void host_gui_toast_frame(unsigned w,unsigned h,host_rect_t area) { (void)w;(void)h;(void)area; }
int host_gui_requested(void) { return requested; }
void host_gui_request(host_gui_pane_t pane) { requested_pane=pane; requested=1; }
host_gui_result_t host_gui_result(void) { return (host_gui_result_t)result; }
void host_gui_close(void) { visible=0; result=HOST_GUI_PLAY; }
void host_gui_open(void) {
    ++opens; requested=0; visible=1; paused_frames=0; result=HOST_GUI_RUNNING;
    assert(host_display_gui_loop(0)==HOST_GUI_PLAY);
}
void host_gui_frame(unsigned w,unsigned h) {
    uint8_t axes[4]; assert(w && h && visible);
    assert(host_input_buttons(port)==0 && !host_input_fast());
    host_input_dualshock(1); host_input_axes(port,axes);
    assert(axes[0]==128 && axes[1]==128 && axes[2]==128 && axes[3]==128);
    host_input_dualshock(0);
    host_input_sync(cpu,&input); reply(0);
    ++paused_frames;
    if (paused_frames==4) host_gui_close();
}
void host_gui_event(const SDL_Event *event) { (void)event; }
const char *host_gui_string(const char *id) { (void)id; return "Soft"; }
int gui_ui_attach(SDL_Window *w,SDL_Renderer *r) { (void)w; return r!=NULL; }
void gui_ui_detach(void) {}
void gui_ui_reset_input(void) {}
void gui_ui_soft_render(SDL_Renderer *r) { assert(r); }
void host_gui_test_event(SDL_Event *event) { (void)event; }
void host_gui_test_tick(uint64_t frame) {
    if (!visible) return;
    if (!paused_frames) paused_frame=frame;
    assert(frame==paused_frame);
    if (paused_frames==1) {
        SDL_Event key; SDL_zero(key); key.type=SDL_KEYDOWN; key.key.keysym.scancode=SDL_SCANCODE_X;
        assert(SDL_PushEvent(&key)==1);
        queue_button(SDL_CONTROLLER_BUTTON_A,1);
        queue_axis(SDL_CONTROLLER_AXIS_LEFTX,32767);
        queue_axis(SDL_CONTROLLER_AXIS_TRIGGERRIGHT,32767);
    }
}
void host_gui_test_observe(void) {}
const char *host_gui_test_capture(void) { return NULL; }
void host_gui_test_captured(int success) { assert(success); }
void host_gui_test_timing(double a,double b,double c) { (void)a;(void)b;(void)c; }
int host_gui_test_active(void) { return 0; }
int host_gui_test_finished(void) { return 1; }
int host_gui_test_failed(void) { return 0; }

int main(void) {
    int virtual_index; unsigned initial;
    cpu=malloc(sizeof(*cpu)); assert(cpu); cpu_init(cpu); sio_init(cpu);
    gpu=gpu_create(); assert(gpu && host_display_init(0,NULL,NULL));
    host_display_gui_audio(pause_audio,resume_audio);
    initial=(unsigned)SDL_NumJoysticks();
    printf("Physical SDL joysticks: %u\n",initial);
    virtual_index=SDL_JoystickAttachVirtual(SDL_JOYSTICK_TYPE_GAMECONTROLLER,6,16,0);
    assert(virtual_index>=0); controller=SDL_JoystickGetDeviceInstanceID(virtual_index); pump();
    for (port=0;port<2 && host_input_controller_id(port)!=controller;++port) {}
    assert(port<2);
    for (unsigned off=0;off<2;++off) {
        unsigned before=opens;
        host_display_hotkeys((int)off,1);
        button(SDL_CONTROLLER_BUTTON_GUIDE,1); assert(opens==before);
        button(SDL_CONTROLLER_BUTTON_RIGHTSHOULDER,1); assert(opens==before);
        if (!off) { assert(host_input_state()==1); reply(0); }
        else reply(PAD_R1);
        button(SDL_CONTROLLER_BUTTON_GUIDE,0); assert(opens==before);
        button(SDL_CONTROLLER_BUTTON_RIGHTSHOULDER,0); reply(0);
        button(SDL_CONTROLLER_BUTTON_A,1); reply(PAD_CROSS);
        button(SDL_CONTROLLER_BUTTON_GUIDE,1); button(SDL_CONTROLLER_BUTTON_GUIDE,0);
        assert(opens==before); reply(PAD_CROSS); button(SDL_CONTROLLER_BUTTON_A,0);
        button(SDL_CONTROLLER_BUTTON_GUIDE,1); button(SDL_CONTROLLER_BUTTON_GUIDE,0);
        assert(opens==before+1 && paused_frames==4 && !visible);
        assert(!host_input_buttons(port)); reply(0);
        queue_button(SDL_CONTROLLER_BUTTON_A,0); queue_axis(SDL_CONTROLLER_AXIS_LEFTX,0);
        queue_axis(SDL_CONTROLLER_AXIS_TRIGGERRIGHT,0); pump(); reply(0);
    }
    assert(pauses==2 && resumes==2 && !host_input_state());
    host_display_hotkeys(0,1);
    button(SDL_CONTROLLER_BUTTON_GUIDE,1);
    button(SDL_CONTROLLER_BUTTON_A,1);
    assert(opens==3 && requested_pane==HOST_GUI_ACHIEVEMENTS);
    reply(0);
    button(SDL_CONTROLLER_BUTTON_A,0); button(SDL_CONTROLLER_BUTTON_GUIDE,0);
    assert(opens==3);
    SDL_Event key; SDL_zero(key); key.type=SDL_KEYDOWN; key.key.keysym.scancode=SDL_SCANCODE_F10;
    assert(SDL_PushEvent(&key)==1); pump();
    assert(opens==4 && requested_pane==HOST_GUI_ACHIEVEMENTS);
    key.type=SDL_KEYUP; assert(SDL_PushEvent(&key)==1); pump(); reply(0);
    assert(pauses==4 && resumes==4);
    assert(SDL_JoystickDetachVirtual(virtual_index)==0); pump();
    assert(host_display_shutdown(gpu)); sio_destroy(cpu); hw_destroy(cpu); free(cpu); free(gpu);
    puts("Guide taps opened; Guide+Cross and F10 opened achievements; paused SIO replies were released; held input cleared");
    return 0;
}
