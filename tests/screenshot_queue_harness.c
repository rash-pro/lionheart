#include "screenshot.h"
#include "osd.h"
#include <SDL.h>
#include <assert.h>
#include <stdint.h>
#include <stdlib.h>

static SDL_sem *started,*ready;
static SDL_threadID owner;
static unsigned last=~0u,release_count;
static uint8_t rgb[16]={255,0,0,0,255,0,0,0,255,255,255,255};
void __real_host_osd_event(unsigned,unsigned,const char *);
void __wrap_host_osd_event(unsigned event,unsigned n,const char *s) {
    assert(SDL_ThreadID()==owner); last=event; __real_host_osd_event(event,n,s);
}
static const uint8_t *read_pixels(void *data) {
    assert(SDL_ThreadID()!=owner && data==rgb);
    SDL_SemPost(started); SDL_SemWait(ready); return rgb;
}
static void release(void *data) { assert(SDL_ThreadID()==owner && data==rgb); ++release_count; }
int main(int argc,char **argv) {
    assert(argc==2); owner=SDL_ThreadID(); started=SDL_CreateSemaphore(0); ready=SDL_CreateSemaphore(0);
    host_osd_configure(1,NULL); assert(host_screenshot_configure(argv[1],"3\n3\n"));
    host_screenshot_frame(2); assert(!host_screenshot_pending());
    host_screenshot_frame(3); assert(host_screenshot_pending());
    host_screenshot_submit(2,2,HOST_SCREENSHOT_RGB,rgb,read_pixels,release);
    assert(!SDL_SemWaitTimeout(started,5000));
    for (unsigned i=0;i<100;++i) { host_screenshot_request(); host_screenshot_frame(4); assert(!host_screenshot_pending()); }
    assert(last==~0u && !release_count);
    SDL_SemPost(ready); host_screenshot_flush(); assert(last==HOST_OSD_SCREENSHOT && release_count==1);
    const uint8_t bgra[16]={0,0,255,255,0,255,0,255,255,0,0,255,255,255,255,255};
    for (unsigned i=0;i<16;++i) rgb[i]=bgra[i];
    host_screenshot_request(); SDL_SemPost(ready);
    host_screenshot_submit(2,2,HOST_SCREENSHOT_BGRA,rgb,read_pixels,release);
    host_screenshot_flush(); assert(last==HOST_OSD_SCREENSHOT && release_count==2);
    host_screenshot_request(); assert(host_screenshot_pending());
    host_screenshot_fail("cannot read"); assert(last==HOST_OSD_SCREENSHOT_FAILED && !host_screenshot_pending());
    host_screenshot_shutdown(); host_osd_shutdown(); SDL_DestroySemaphore(started); SDL_DestroySemaphore(ready); return 0;
}
