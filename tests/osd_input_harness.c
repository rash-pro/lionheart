#include "display.h"
#include "osd.h"
#include "states.h"
#ifdef FF8_VULKAN
#include "vulkan_renderer.h"
#endif
#include <SDL.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned last=~0u, number;
static char detail[512];
void __real_host_osd_event(unsigned,unsigned,const char *);
void __wrap_host_osd_event(unsigned event,unsigned n,const char *s) {
    last=event; number=n; snprintf(detail,sizeof(detail),"%s",s ? s : "");
    __real_host_osd_event(event,n,s);
}
static void key(gpu_t *g,SDL_Scancode code,int down,int repeat) {
    SDL_Event e; SDL_zero(e); e.type=down ? SDL_KEYDOWN : SDL_KEYUP;
    e.key.keysym.scancode=code; e.key.repeat=(Uint8)repeat; last=~0u;
    assert(SDL_PushEvent(&e)==1); host_present(g);
}
int main(int argc,char **argv) {
    gpu_t *g=gpu_create(); int i, pad, vulkan=argc>1;
    assert(g); (void)argv;
    assert(host_display_init(0,NULL,NULL));
    printf("Physical SDL joysticks: %d\n",SDL_NumJoysticks());
    for (i=0;i<SDL_NumJoysticks();++i) printf("device %d: %s\n",i,SDL_JoystickNameForIndex(i));
#ifdef FF8_VULKAN
    if (vulkan) assert(host_gpu_configure("vulkan",1,0,1,NULL,NULL) && host_gpu_start(g,NULL) && host_gpu_enabled());
#else
    assert(!vulkan);
#endif
    host_osd_configure(1,NULL);
    for (i=0;i<4;++i) {
        key(g,SDL_SCANCODE_F9,1,0);
        assert(last==(vulkan ? HOST_OSD_CRT : HOST_OSD_CRT_SOFT) && number==(unsigned)((i+1)%4));
    }
    key(g,SDL_SCANCODE_F9,1,1); assert(last==~0u);
    key(g,SDL_SCANCODE_TAB,1,0); assert(last==HOST_OSD_FAST && host_input_fast());
    key(g,SDL_SCANCODE_TAB,0,0); assert(!host_input_fast());
    for (i=0;i<4;++i) {
        key(g,(SDL_Scancode)(SDL_SCANCODE_F1+i),1,0); assert(host_input_disc()==(unsigned)i+1);
        assert(last==HOST_OSD_DISC && number==(unsigned)i+1);
    }
    key(g,SDL_SCANCODE_F11,1,0); assert(last==~0u);
    host_display_ra(1,1);
    key(g,SDL_SCANCODE_F7,1,0); assert(last==HOST_OSD_RA_LOAD && !host_input_state());
    key(g,SDL_SCANCODE_TAB,1,0); assert(last==HOST_OSD_RA_FAST && !host_input_fast());
    key(g,SDL_SCANCODE_TAB,1,1); assert(last==~0u);
    key(g,SDL_SCANCODE_F9,1,0); assert(last==(vulkan ? HOST_OSD_CRT : HOST_OSD_CRT_SOFT));
    host_display_ra(0,0);
    assert(host_states_init(NULL,"/dev/null/states",NULL,NULL,NULL,0,NULL,0,0,0,0,0));
    key(g,SDL_SCANCODE_F6,1,0); host_states_vblank(NULL); assert(last==HOST_OSD_UNAVAILABLE);
    key(g,SDL_SCANCODE_F8,1,0); host_states_vblank(NULL); assert(last==HOST_OSD_UNAVAILABLE);
    key(g,SDL_SCANCODE_F5,1,0); host_states_vblank(NULL); assert(last==HOST_OSD_UNAVAILABLE);
    key(g,SDL_SCANCODE_F7,1,0); host_states_vblank(NULL); assert(last==HOST_OSD_UNAVAILABLE);
    pad=SDL_JoystickAttachVirtual(SDL_JOYSTICK_TYPE_GAMECONTROLLER,6,16,0);
    assert(pad>=0); last=~0u; host_present(g);
    assert(last==HOST_OSD_CONNECTED && *detail);
    assert(!SDL_JoystickDetachVirtual(pad)); last=~0u; host_present(g);
    assert(last==HOST_OSD_DISCONNECTED);
    assert(host_display_shutdown(g)); free(g); return 0;
}
