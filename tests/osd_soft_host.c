/* Exercise the actual SDL presenter offscreen, including output-pixel placement. */
#include "display.h"
#include "osd.h"
#include <SDL.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

static int width,height;
static double age;
static const char *path;
SDL_Window *__real_SDL_CreateWindow(const char *,int,int,int,int,Uint32);
SDL_Window *__wrap_SDL_CreateWindow(const char *title,int x,int y,int w,int h,Uint32 flags) {
    (void)w; (void)h; return __real_SDL_CreateWindow(title,x,y,width,height,flags|SDL_WINDOW_HIDDEN);
}
void __real_host_osd_frame(int,uint64_t);
void __wrap_host_osd_frame(int window,uint64_t frame) {
    (void)window; __real_host_osd_frame(0,frame); host_osd_clock(age);
}
void __real_SDL_RenderPresent(SDL_Renderer *);
void __wrap_SDL_RenderPresent(SDL_Renderer *r) {
    uint8_t *rgb=malloc((size_t)width*height*3); FILE *f;
    assert(rgb && !SDL_RenderReadPixels(r,NULL,SDL_PIXELFORMAT_RGB24,rgb,width*3));
    f=fopen(path,"wb"); assert(f && fwrite(rgb,3,(size_t)width*height,f)==(size_t)width*height);
    assert(!fclose(f)); free(rgb); __real_SDL_RenderPresent(r);
}
int main(int argc,char **argv) {
    gpu_t *g=gpu_create();
    assert(argc==7 && g); width=atoi(argv[1]); height=atoi(argv[2]); path=argv[3]; age=atof(argv[6]);
    assert(host_display_init(0,NULL,NULL));
    gpu_gp1(g,0x06000000|600|(3160u<<12)); gpu_gp1(g,0x07000000|16|(256u<<10));
    gpu_gp1(g,0x08000001); gpu_gp1(g,0x03000000);
    gpu_gp0(g,0x02808080); gpu_gp0(g,0); gpu_gp0(g,320|(240u<<16));
    host_osd_configure(1,NULL); host_osd_clock(0);
    if (*argv[4]) host_osd_post(argv[4],argv[5],0);
    host_present(g);
    assert(host_display_shutdown(g)); free(g); return 0;
}
