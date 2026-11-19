#include "osd.h"
#include <SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "osd_font.h"

static const struct { const char *label, *value[4]; int persistent; } copy[] = {
    {"CRT",{"Off","PVM","TV","Arcade"},0}, {"CRT",{"needs the Vulkan renderer"},0},
    {"State slot",{"%u","%u (empty)"},0}, {"Saving",{"slot %u..."},1}, {"State saved",{"slot %u"},0},
    {"Save failed",{"slot %u","slot %u: %.48s"},0}, {"Loading",{"slot %u..."},1}, {"State loaded",{"slot %u"},0},
    {"No state",{"in slot %u"},0}, {"Load failed",{"slot %u","slot %u: %.48s"},0},
    {"Memory card",{"changed: reopen the save menu"},0}, {"RA strict",{"loading states is off"},0},
    {"RA strict",{"fast-forward is off"},0}, {"Fast forward",{""},1}, {"Disc",{"%u"},0},
    {"Controller",{"connected: %s"},0}, {"Controller",{"disconnected"},0}, {"Save states",{"not available"},0},
    {"Screenshot",{"saved"},0}, {"Screenshot failed",{"%.48s"},0}
};
static struct {
    int enabled, window, persistent, replacement, dirty, width, height;
    int paused, saved_window;
    unsigned event;
    double now, start, clock_offset;
    char label[512], value[512];
    struct { uint64_t frame; char label[512], value[512]; int fired; } tests[32];
    unsigned count;
    host_osd_panel_t panel;
    uint8_t *pixels;
} osd = {.enabled=1};

static double now(void) {
    return osd.window ? SDL_GetPerformanceCounter()*1000.0/SDL_GetPerformanceFrequency() : osd.now;
}
double host_osd_now(void) { return now()+osd.clock_offset; }
static uint8_t opacity(void) {
    double age=now()-osd.start, a;
    if (!osd.enabled || !*osd.label) return 0;
    a=osd.replacement || age>=120 ? 1 : age/120;
    if (!osd.persistent && age>=1920) a=(2220-age)/300;
    return a<=0 ? 0 : a>=1 ? 255 : (uint8_t)(a*255+0.5);
}
static void shutdown_locked(void) { free(osd.pixels); memset(&osd,0,sizeof(osd)); osd.enabled=1; }
static void configure_locked(int enabled,const char *tests) {
    const char *p=tests;
    shutdown_locked(); osd.enabled=enabled;
    while (p && *p && osd.count<32) {
        char *parsed; const char *end; size_t n; unsigned i=osd.count++;
        osd.tests[i].frame=strtoull(p,&parsed,10); end=parsed;
        if (*end!=':') { --osd.count; break; }
        p=end+1; end=strpbrk(p,":\n"); n=end ? (size_t)(end-p) : strlen(p);
        if (n>511) n=511;
        memcpy(osd.tests[i].label,p,n);
        p+=n;
        if (*p==':') {
            ++p; end=strchr(p,'\n'); n=end ? (size_t)(end-p) : strlen(p);
            if (n>511) n=511;
            memcpy(osd.tests[i].value,p,n); p+=n;
        }
        if (*p=='\n') ++p;
    }
}
static void clock_locked(double milliseconds) { osd.window=0; osd.now=milliseconds; }
static void pause_locked(int paused) {
    double age,stamp;
    if (!!paused==osd.paused) return;
    age=now()-osd.start; stamp=host_osd_now();
    if (paused) { osd.saved_window=osd.window; osd.window=1; }
    else osd.window=osd.saved_window;
    osd.start=now()-age; osd.clock_offset=stamp-now(); osd.paused=!!paused;
}
static void post_locked(const char *label,const char *value,int persistent);
static void frame_locked(int window,uint64_t frame) {
    unsigned i;
    osd.window=window; osd.now=frame*(1000.0/60);
    for (i=0;i<osd.count;++i) if (!osd.tests[i].fired && frame>=osd.tests[i].frame) {
        osd.tests[i].fired=1;
        post_locked(osd.tests[i].label,osd.tests[i].value,0);
    }
}
static void post_locked(const char *label,const char *value,int persistent) {
    int replace=opacity()!=0;
    if (!value) value="";
    if (strcmp(osd.label,label) || strcmp(osd.value,value)) osd.dirty=1;
    snprintf(osd.label,sizeof(osd.label),"%s",label);
    snprintf(osd.value,sizeof(osd.value),"%s",value);
    osd.persistent=persistent; osd.replacement=replace; osd.start=now(); osd.event=~0u;
}
static void event_locked(unsigned event,unsigned number,const char *detail) {
    char value[512]; const char *format;
    if (event>=sizeof(copy)/sizeof(copy[0])) return;
    format=copy[event].value[0];
    if (event==HOST_OSD_CRT) snprintf(value,sizeof(value),"%s",copy[event].value[number<4 ? number : 0]);
    else if (event==HOST_OSD_SLOT) snprintf(value,sizeof(value),copy[event].value[detail!=NULL],number);
    else if (event==HOST_OSD_CONNECTED || event==HOST_OSD_SCREENSHOT_FAILED) snprintf(value,sizeof(value),format,detail ? detail : "?");
    else if (event==HOST_OSD_SAVE_FAILED || event==HOST_OSD_LOAD_FAILED)
        snprintf(value,sizeof(value),copy[event].value[detail && *detail ? 1 : 0],number,detail);
    else if (event==HOST_OSD_SAVING || event==HOST_OSD_SAVED || event==HOST_OSD_LOADING ||
             event==HOST_OSD_LOADED || event==HOST_OSD_NO_STATE || event==HOST_OSD_DISC)
        snprintf(value,sizeof(value),format,number);
    else snprintf(value,sizeof(value),"%s",format);
    post_locked(copy[event].label,value,copy[event].persistent); osd.event=event;
}
static const char *showing_locked(void) { return opacity() ? osd.label : ""; }
static void clear_locked(unsigned event) {
    if (osd.event==event) { *osd.label=0; osd.dirty=1; }
}
/* UTF-8 controller names; unsupported codepoints become one '?' each. */
static unsigned glyph(const char **text) {
    const unsigned char *p=(const unsigned char *)*text;
    unsigned c=*p++, n=0, minimum=0;
    if (c>=0xc2 && c<=0xdf) { c&=31; n=1; minimum=128; }
    else if (c>=0xe0 && c<=0xef) { c&=15; n=2; minimum=2048; }
    else if (c>=0xf0 && c<=0xf4) { c&=7; n=3; minimum=65536; }
    else if (c>=128) c=63;
    while (n--) {
        if ((*p&0xc0)!=0x80) { c=63; break; }
        c=(c<<6)|(*p++&63);
    }
    *text=(const char *)p;
    if (c<minimum || c<32 || (c>=127 && c<160) || c>255) c=63;
    return c-32;
}
static int cells(const char *s) { int n=0; while (*s) { glyph(&s); ++n; } return n; }
static void text(const char *s,int x,int y,int width,int height,int scale,const uint32_t *font,const uint8_t color[3]) {
    int row,col,dy,dx;
    while (*s) {
        unsigned c=glyph(&s);
        for (row=0;row<height;++row) for (col=0;col<width;++col) if (font[c*height+row]&(0x80000000u>>col))
            for (dy=0;dy<scale;++dy) for (dx=0;dx<scale;++dx) {
                uint8_t *p=osd.pixels+((size_t)(y+row*scale+dy)*osd.panel.w+x+col*scale+dx)*4;
                memcpy(p,color,3); p[3]=255;
            }
        x+=width*scale;
    }
}
host_rect_t host_osd_area(host_viewport_t v) {
    int x0=v.content.x>v.frame.x ? v.content.x : v.frame.x, y0=v.content.y>v.frame.y ? v.content.y : v.frame.y;
    int x1=v.content.x+v.content.w<v.frame.x+v.frame.w ? v.content.x+v.content.w : v.frame.x+v.frame.w;
    int y1=v.content.y+v.content.h<v.frame.y+v.frame.h ? v.content.y+v.content.h : v.frame.y+v.frame.h;
    host_rect_t area={x0,y0,x1-x0,y1-y0};
    return area.w>0 && area.h>0 ? area : v.frame;
}
static const int cell_heights[]={16,24,32,48,64};
static int font_index(int height) {
    int chosen=0,best=abs(cell_heights[0]*45-height);
    for (int i=1;i<5;++i) {
        int distance=abs(cell_heights[i]*45-height);
        if (distance<best) { best=distance; chosen=i; }
    }
    return chosen;
}
float host_osd_corner_radius(int height) { return cell_heights[font_index(height)]*0.3f; }
static const host_osd_panel_t *panel_locked(int width,int height,host_rect_t area) {
    static const int widths[]={8,12,16,12,32};
    static const uint8_t dim[]={165,165,175}, bright[]={242,242,242};
    const uint32_t *fonts[]={&spleen_16[0][0],&spleen_24[0][0],&spleen_32[0][0],&spleen_24[0][0],&spleen_64[0][0]};
    int chosen,padx,pady,cw,ch,scale,x,y;
    uint8_t a=opacity();
    if (!a || width<=0 || height<=0) return NULL;
    osd.panel.opacity=a;
    osd.panel.x=area.x+(int)(area.h*0.03+0.5); osd.panel.y=area.y+(int)(area.h*0.03+0.5);
    if (!osd.dirty && width==osd.width && height==osd.height) return &osd.panel;
    chosen=font_index(height);
    ch=cell_heights[chosen]; scale=chosen==3 ? 2 : 1; cw=widths[chosen]*scale;
    padx=(int)(ch*0.75+0.5); pady=(int)(ch*0.4+0.5);
    osd.panel.w=2*padx+(cells(osd.label)+(*osd.value ? 1+cells(osd.value) : 0))*cw;
    osd.panel.h=2*pady+ch; osd.panel.cell_w=cw; osd.panel.cell_h=ch;
    free(osd.pixels); osd.pixels=malloc((size_t)osd.panel.w*osd.panel.h*4);
    osd.panel.rgba=osd.pixels;
    if (!osd.pixels) { osd.dirty=1; return NULL; }
    for (y=0;y<osd.panel.h;++y) for (x=0;x<osd.panel.w;++x) {
        float r=ch*0.3f; int sx,sy,inside=0;
        uint8_t *p=osd.pixels+((size_t)y*osd.panel.w+x)*4;
        for (sy=0;sy<4;++sy) for (sx=0;sx<4;++sx) {
            float px=x+(sx+0.5f)/4, py=y+(sy+0.5f)/4;
            float dx=px<r ? r-px : px>osd.panel.w-r ? px-(osd.panel.w-r) : 0;
            float dy=py<r ? r-py : py>osd.panel.h-r ? py-(osd.panel.h-r) : 0;
            inside+=dx*dx+dy*dy<=r*r;
        }
        p[0]=p[1]=16; p[2]=20; p[3]=(uint8_t)((199*inside+8)/16);
    }
    text(osd.label,padx,pady,widths[chosen],ch/scale,scale,fonts[chosen],*osd.value ? dim : bright);
    if (*osd.value) text(osd.value,padx+(cells(osd.label)+1)*cw,pady,widths[chosen],ch/scale,scale,fonts[chosen],bright);
    osd.width=width; osd.height=height; osd.dirty=0; ++osd.panel.revision;
    return &osd.panel;
}

/* Smooth motion presents from a thread of its own, which reads what the game
 * thread posts: every entry takes one lock, and a renderer holds it while it
 * uses the panel it was given. */
static SDL_SpinLock guard;
static SDL_mutex *mutex;
void host_osd_hold(int on) {
    if (!mutex) {
        SDL_AtomicLock(&guard);
        if (!mutex) mutex = SDL_CreateMutex();
        SDL_AtomicUnlock(&guard);
    }
    if (!mutex) return;
    if (on) SDL_LockMutex(mutex); else SDL_UnlockMutex(mutex);
}
void host_osd_shutdown(void) { host_osd_hold(1); shutdown_locked(); host_osd_hold(0); }
void host_osd_configure(int enabled,const char *tests) { host_osd_hold(1); configure_locked(enabled,tests); host_osd_hold(0); }
void host_osd_clock(double milliseconds) { host_osd_hold(1); clock_locked(milliseconds); host_osd_hold(0); }
void host_osd_pause(int paused) { host_osd_hold(1); pause_locked(paused); host_osd_hold(0); }
void host_osd_frame(int window,uint64_t frame) { host_osd_hold(1); frame_locked(window,frame); host_osd_hold(0); }
void host_osd_post(const char *label,const char *value,int persistent) { host_osd_hold(1); post_locked(label,value,persistent); host_osd_hold(0); }
void host_osd_event(unsigned event,unsigned number,const char *detail) { host_osd_hold(1); event_locked(event,number,detail); host_osd_hold(0); }
const char *host_osd_showing(void) { const char *s; host_osd_hold(1); s=showing_locked(); host_osd_hold(0); return s; }
void host_osd_clear(unsigned event) { host_osd_hold(1); clear_locked(event); host_osd_hold(0); }
const host_osd_panel_t *host_osd_panel(int width,int height,host_rect_t area) {
    const host_osd_panel_t *p; host_osd_hold(1); p=panel_locked(width,height,area); host_osd_hold(0); return p;
}
