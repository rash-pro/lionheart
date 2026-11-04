#include "display.h"
#include "input.h"
#include "osd.h"
#include "screenshot.h"
#include "hw.h"
#include <SDL.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static cpu_t *c;
static gpu_t *g;
static host_input_t input;
static SDL_JoystickID ids[2];
static unsigned last,number;
static unsigned missing_mask;
static SDL_Window *window;
SDL_Window *__real_SDL_CreateWindow(const char *,int,int,int,int,Uint32);
SDL_Window *__wrap_SDL_CreateWindow(const char *title,int x,int y,int w,int h,Uint32 flags) {
    window=__real_SDL_CreateWindow(title,x,y,w,h,flags); return window;
}
void __real_host_osd_event(unsigned,unsigned,const char *);
void __wrap_host_osd_event(unsigned event,unsigned n,const char *s) {
    last=event; number=n; __real_host_osd_event(event,n,s);
}
void __real_host_osd_post(const char *,const char *,int);
void __wrap_host_osd_post(const char *label,const char *value,int persistent) {
    unsigned disc;
    if (sscanf(label,"Disc %u missing",&disc)==1 && disc>=1 && disc<=4) missing_mask|=1u<<(disc-1);
    __real_host_osd_post(label,value,persistent);
}
static void pump(void) { host_present(g); host_input_sync(c,&input); }
static void button(unsigned port,unsigned button,int down) {
    SDL_Event e; SDL_zero(e); e.type=down ? SDL_CONTROLLERBUTTONDOWN : SDL_CONTROLLERBUTTONUP;
    e.cbutton.which=ids[port]; e.cbutton.button=(Uint8)button;
    assert(SDL_PushEvent(&e)==1); pump();
}
static void axis(unsigned port,unsigned axis,int value) {
    SDL_Event e; SDL_zero(e); e.type=SDL_CONTROLLERAXISMOTION;
    e.caxis.which=ids[port]; e.caxis.axis=(Uint8)axis; e.caxis.value=(Sint16)value;
    assert(SDL_PushEvent(&e)==1); pump();
}
static void key(unsigned code,int down) {
    SDL_Event e; SDL_zero(e); e.type=down ? SDL_KEYDOWN : SDL_KEYUP; e.key.keysym.scancode=(SDL_Scancode)code;
    assert(SDL_PushEvent(&e)==1); pump();
}
static unsigned wire(unsigned value) {
    hw_write(c,0x1f801040,1,value); c->cycles+=2000; hw_event(c); return hw_read(c,0x1f801040,1);
}
static void guest(unsigned port,uint16_t expected) {
    unsigned low,high;
    hw_write(c,0x1f80104a,2,0); hw_write(c,0x1f801048,2,0xd); hw_write(c,0x1f80104e,2,136);
    hw_write(c,0x1f80104a,2,3|(port<<13));
    assert(wire(1)==255); assert(wire(0x42)==0x41); assert(wire(0)==0x5a);
    low=wire(0); high=wire(0);
    assert(((low|(high<<8))^65535u)==expected);
}
int main(int argc,char **argv) {
    int devices[2],i;
    assert(argc==2);
    c=malloc(sizeof(*c)); assert(c); cpu_init(c); sio_init(c); g=gpu_create(); assert(g);
    assert(host_display_init(0,NULL,NULL)); host_osd_configure(1,NULL);
    assert(host_screenshot_configure(argv[1],NULL));
    printf("Physical SDL joysticks: %d\n",SDL_NumJoysticks());
    for (i=0;i<SDL_NumJoysticks();++i) printf("device %d: %s\n",i,SDL_JoystickNameForIndex(i));
    for (i=0;i<2;++i) {
        devices[i]=SDL_JoystickAttachVirtual(SDL_JOYSTICK_TYPE_GAMECONTROLLER,6,16,0); assert(devices[i]>=0); pump();
        ids[i]=SDL_JoystickGetDeviceInstanceID(devices[i]);
        assert(host_input_controller_id((unsigned)i)==ids[i]);
    }
    host_display_hotkeys(0,3);
    for (unsigned p=0;p<2;++p) {
        const unsigned buttons[]={SDL_CONTROLLER_BUTTON_RIGHTSHOULDER,SDL_CONTROLLER_BUTTON_LEFTSHOULDER,
            SDL_CONTROLLER_BUTTON_DPAD_LEFT,SDL_CONTROLLER_BUTTON_DPAD_RIGHT};
        for (unsigned n=0;n<4;++n) {
            button(p,SDL_CONTROLLER_BUTTON_GUIDE,1); button(p,buttons[n],1);
            assert(host_input_state()==n+1); guest(p,0); guest(1-p,0);
            button(p,SDL_CONTROLLER_BUTTON_GUIDE,0); guest(p,0);
            button(p,buttons[n],0); guest(p,0);
        }
        button(p,SDL_CONTROLLER_BUTTON_A,1); guest(p,PAD_CROSS);
        button(p,SDL_CONTROLLER_BUTTON_GUIDE,1); guest(p,PAD_CROSS);
        button(p,SDL_CONTROLLER_BUTTON_B,1); guest(p,PAD_CROSS);
        button(p,SDL_CONTROLLER_BUTTON_GUIDE,0); guest(p,PAD_CROSS);
        button(p,SDL_CONTROLLER_BUTTON_B,0); button(p,SDL_CONTROLLER_BUTTON_A,0); guest(p,0);
        button(p,SDL_CONTROLLER_BUTTON_GUIDE,1);
        /* Every unused button is swallowed, including L2. */
        const unsigned unused[]={SDL_CONTROLLER_BUTTON_A,SDL_CONTROLLER_BUTTON_B,SDL_CONTROLLER_BUTTON_LEFTSTICK,
            SDL_CONTROLLER_BUTTON_RIGHTSTICK,SDL_CONTROLLER_BUTTON_DPAD_UP,SDL_CONTROLLER_BUTTON_DPAD_DOWN};
        for (unsigned n=0;n<sizeof(unused)/sizeof(*unused);++n) { button(p,unused[n],1); guest(p,0); button(p,unused[n],0); }
        axis(p,SDL_CONTROLLER_AXIS_TRIGGERLEFT,16001); guest(p,0);
        axis(p,SDL_CONTROLLER_AXIS_TRIGGERLEFT,16000); guest(p,0);
        axis(p,SDL_CONTROLLER_AXIS_TRIGGERRIGHT,16000); assert(!host_input_fast());
        axis(p,SDL_CONTROLLER_AXIS_TRIGGERRIGHT,16001); assert(host_input_fast() && last==HOST_OSD_FAST); guest(p,0);
        button(p,SDL_CONTROLLER_BUTTON_GUIDE,0); assert(host_input_fast()); guest(p,0);
        key(SDL_SCANCODE_TAB,1); axis(p,SDL_CONTROLLER_AXIS_TRIGGERRIGHT,0); assert(host_input_fast());
        key(SDL_SCANCODE_TAB,0); assert(!host_input_fast()); guest(p,0);
        axis(p,SDL_CONTROLLER_AXIS_TRIGGERRIGHT,32767); guest(p,PAD_R2);
        button(p,SDL_CONTROLLER_BUTTON_GUIDE,1); guest(p,PAD_R2); assert(!host_input_fast());
        axis(p,SDL_CONTROLLER_AXIS_TRIGGERRIGHT,0); guest(p,0);
        axis(p,SDL_CONTROLLER_AXIS_LEFTX,32767); guest(p,PAD_RIGHT); /* sticks unaffected */
        button(p,SDL_CONTROLLER_BUTTON_DPAD_RIGHT,1); assert(host_input_state()==4); guest(p,PAD_RIGHT);
        button(p,SDL_CONTROLLER_BUTTON_DPAD_RIGHT,0); guest(p,PAD_RIGHT);
        host_input_dualshock(1);
        uint8_t sticks[4]; host_input_axes(p,sticks); assert(sticks[2]==255);
        host_input_dualshock(0); axis(p,SDL_CONTROLLER_AXIS_LEFTX,0);
        for (unsigned disc=1;disc<=3;++disc) {
            host_display_disc(disc); button(p,SDL_CONTROLLER_BUTTON_BACK,1);
            assert(host_input_disc()==disc%3+1 && last==HOST_OSD_DISC && number==disc%3+1); guest(p,0);
            button(p,SDL_CONTROLLER_BUTTON_BACK,0);
        }
        host_display_disc_mask(1u | 8u);
        missing_mask=0;
        host_display_disc(1); button(p,SDL_CONTROLLER_BUTTON_BACK,1);
        assert(host_input_disc()==4 && missing_mask==(2u|4u)); button(p,SDL_CONTROLLER_BUTTON_BACK,0);
        host_display_disc(4); button(p,SDL_CONTROLLER_BUTTON_BACK,1);
        assert(host_input_disc()==1); button(p,SDL_CONTROLLER_BUTTON_BACK,0);
        host_display_disc_mask(7u);
        int crt=host_display_crt_preset(); button(p,SDL_CONTROLLER_BUTTON_Y,1);
        assert(host_display_crt_preset()==(crt+1)%4 && last==HOST_OSD_CRT_SOFT); guest(p,0);
        button(p,SDL_CONTROLLER_BUTTON_Y,0);
        SDL_Window *w=window; assert(w);
        Uint32 before=SDL_GetWindowFlags(w)&SDL_WINDOW_FULLSCREEN_DESKTOP;
        button(p,SDL_CONTROLLER_BUTTON_START,1); guest(p,0);
        assert((SDL_GetWindowFlags(w)&SDL_WINDOW_FULLSCREEN_DESKTOP)!=before);
        button(p,SDL_CONTROLLER_BUTTON_START,0);
        host_display_ra(1,1);
        button(p,SDL_CONTROLLER_BUTTON_LEFTSHOULDER,1); assert(last==HOST_OSD_RA_LOAD && !host_input_state()); guest(p,0);
        button(p,SDL_CONTROLLER_BUTTON_LEFTSHOULDER,0);
        axis(p,SDL_CONTROLLER_AXIS_TRIGGERRIGHT,32767); assert(last==HOST_OSD_RA_FAST && !host_input_fast()); guest(p,0);
        axis(p,SDL_CONTROLLER_AXIS_TRIGGERRIGHT,0);
        key(SDL_SCANCODE_F7,1); assert(last==HOST_OSD_RA_LOAD && !host_input_state()); key(SDL_SCANCODE_F7,0);
        key(SDL_SCANCODE_TAB,1); assert(last==HOST_OSD_RA_FAST && !host_input_fast()); key(SDL_SCANCODE_TAB,0);
        /* Both screenshot bindings work in strict mode. */
        if (!p) button(p,SDL_CONTROLLER_BUTTON_X,1); else key(SDL_SCANCODE_F12,1);
        host_screenshot_flush(); assert(last==HOST_OSD_SCREENSHOT);
        if (!p) button(p,SDL_CONTROLLER_BUTTON_X,0); else key(SDL_SCANCODE_F12,0);
        host_display_ra(0,0); button(p,SDL_CONTROLLER_BUTTON_GUIDE,0);
    }
    /* A modifier on one port does not consume the other port. */
    button(0,SDL_CONTROLLER_BUTTON_GUIDE,1); button(1,SDL_CONTROLLER_BUTTON_A,1); guest(1,PAD_CROSS);
    button(1,SDL_CONTROLLER_BUTTON_A,0); button(0,SDL_CONTROLLER_BUTTON_GUIDE,0);
    host_display_hotkeys(1,3); button(0,SDL_CONTROLLER_BUTTON_GUIDE,1);
    button(0,SDL_CONTROLLER_BUTTON_RIGHTSHOULDER,1); guest(0,PAD_R1); assert(!host_input_state());
    axis(0,SDL_CONTROLLER_AXIS_TRIGGERRIGHT,32767); guest(0,PAD_R1|PAD_R2); assert(!host_input_fast());
    axis(0,SDL_CONTROLLER_AXIS_TRIGGERLEFT,32767); guest(0,PAD_R1|PAD_R2|PAD_L2);
    axis(0,SDL_CONTROLLER_AXIS_TRIGGERLEFT,0);
    button(0,SDL_CONTROLLER_BUTTON_RIGHTSHOULDER,0); axis(0,SDL_CONTROLLER_AXIS_TRIGGERRIGHT,0);
    const unsigned pass_buttons[]={SDL_CONTROLLER_BUTTON_A,SDL_CONTROLLER_BUTTON_B,SDL_CONTROLLER_BUTTON_X,SDL_CONTROLLER_BUTTON_Y,
        SDL_CONTROLLER_BUTTON_BACK,SDL_CONTROLLER_BUTTON_START,SDL_CONTROLLER_BUTTON_LEFTSTICK,SDL_CONTROLLER_BUTTON_RIGHTSTICK,
        SDL_CONTROLLER_BUTTON_LEFTSHOULDER,SDL_CONTROLLER_BUTTON_RIGHTSHOULDER,SDL_CONTROLLER_BUTTON_DPAD_UP,
        SDL_CONTROLLER_BUTTON_DPAD_DOWN,SDL_CONTROLLER_BUTTON_DPAD_LEFT,SDL_CONTROLLER_BUTTON_DPAD_RIGHT};
    const uint16_t pass_bits[]={PAD_CROSS,PAD_CIRCLE,PAD_SQUARE,PAD_TRIANGLE,PAD_SELECT,PAD_START,PAD_L3,PAD_R3,
        PAD_L1,PAD_R1,PAD_UP,PAD_DOWN,PAD_LEFT,PAD_RIGHT};
    for (unsigned n=0;n<sizeof(pass_buttons)/sizeof(*pass_buttons);++n) {
        button(0,pass_buttons[n],1); guest(0,pass_bits[n]); button(0,pass_buttons[n],0); guest(0,0);
    }
    assert(!host_input_state() && !host_input_disc());
    button(0,SDL_CONTROLLER_BUTTON_GUIDE,0);
    /* Script port 1 stays digital and still permits physical host hotkeys. */
    char error[256],script[4096]; snprintf(script,sizeof(script),"%s/script.txt",argv[1]);
    FILE *f=fopen(script,"w"); assert(f); fputs("0 CROSS\n",f); fclose(f);
    assert(sio_load_script(c,script,error,sizeof(error))); input.scripted=1;
    host_display_hotkeys(0,3); button(0,SDL_CONTROLLER_BUTTON_GUIDE,1);
    button(0,SDL_CONTROLLER_BUTTON_RIGHTSHOULDER,1); assert(host_input_state()==1); guest(0,PAD_CROSS);
    button(0,SDL_CONTROLLER_BUTTON_RIGHTSHOULDER,0); button(0,SDL_CONTROLLER_BUTTON_GUIDE,0);
    button(0,SDL_CONTROLLER_BUTTON_GUIDE,1); axis(0,SDL_CONTROLLER_AXIS_TRIGGERRIGHT,32767); assert(host_input_fast());
    SDL_Event lost; SDL_zero(lost); lost.type=SDL_WINDOWEVENT; lost.window.event=SDL_WINDOWEVENT_FOCUS_LOST;
    assert(SDL_PushEvent(&lost)==1); pump(); assert(!host_input_fast() && !host_input_buttons(0));
    button(1,SDL_CONTROLLER_BUTTON_GUIDE,1); axis(1,SDL_CONTROLLER_AXIS_TRIGGERRIGHT,32767); assert(host_input_fast());
    for (i=1;i>=0;--i) { assert(!SDL_JoystickDetachVirtual(devices[i])); pump(); assert(!host_input_fast()); }
    assert(host_display_shutdown(g)); sio_destroy(c); hw_destroy(c); free(c); free(g); return 0;
}
