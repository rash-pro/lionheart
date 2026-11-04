#include "display.h"
#include "options.h"
#include "osd.h"
#include "screenshot.h"
#ifdef FF8_VULKAN
#include "vulkan_renderer.h"
#endif
#include <SDL.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static unsigned last=~0u;
void __real_host_osd_event(unsigned,unsigned,const char *);
void __wrap_host_osd_event(unsigned event,unsigned number,const char *detail) {
    last=event; __real_host_osd_event(event,number,detail);
}
time_t __wrap_time(time_t *out) { time_t fixed=1700000000; if (out) *out=fixed; return fixed; }
int main(int argc,char **argv) {
    char dir[4096],shots[4096]; int vk,w,h,mode,crt,linear;
    gpu_t *g=gpu_create(); assert(argc==9 && g);
    vk=!strcmp(argv[1],"vulkan"); w=atoi(argv[2]); h=atoi(argv[3]); mode=atoi(argv[4]); crt=atoi(argv[5]); linear=atoi(argv[6]);
    snprintf(dir,sizeof(dir),"%s/presented",argv[7]); snprintf(shots,sizeof(shots),"%s/shots",argv[7]);
#ifdef FF8_VULKAN
    assert(host_gpu_configure(vk ? "vulkan" : "soft",vk ? 4 : 1,0,1,NULL,NULL));
    host_gpu_present_capture(vk ? dir : NULL,(unsigned)w,(unsigned)h);
#else
    assert(!vk);
#endif
    assert(host_display_init(1,NULL,NULL));
    assert(host_display_present_capture(dir,(unsigned)w,(unsigned)h));
    assert(host_display_configure(0,mode,linear)); host_display_crt(crt);
    assert(host_screenshot_configure(atoi(argv[8])==1 ? "/dev/null/no-shots" : shots,NULL));
    host_osd_configure(atoi(argv[8])!=2,NULL);
#ifdef FF8_VULKAN
    assert(host_display_gpu(g));
#endif
    gpu_gp1(g,0x06000000|600|(3160u<<12)); gpu_gp1(g,0x07000000|16|(240u<<10));
    gpu_gp1(g,0x08000001); gpu_gp1(g,0x03000000);
    gpu_gp0(g,0x024080c0); gpu_gp0(g,0); gpu_gp0(g,320|(224u<<16));
    gpu_gp0(g,0x02e0a040); gpu_gp0(g,64|(40u<<16)); gpu_gp0(g,96|(112u<<16));
    host_present(g); /* reference, without OSD */
    host_osd_post("Must not be saved","visible OSD",1);
    host_screenshot_request(); host_screenshot_request(); host_present(g);
    host_screenshot_flush();
    assert(last==(atoi(argv[8])==1 ? HOST_OSD_SCREENSHOT_FAILED : HOST_OSD_SCREENSHOT));
    host_screenshot_request(); host_present(g); host_screenshot_flush();
    assert(host_display_shutdown(g)); free(g); return 0;
}
