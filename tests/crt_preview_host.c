/* Presents a supplied scaled scanout (raw RGB) through the host CRT passes
 * offscreen and writes the result as raw RGB. Used by tests and look tuning. */
#include "display.h"
#include "osd.h"
#include "options.h"
#include "vulkan_renderer.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int usage(void) {
    fprintf(stderr,"usage: crt_preview IN.rgb WIDTH HEIGHT --scale N --native W H --timing H V MODE "
                   "--size WxH --crt off|pvm|tv|arcade --out OUT.rgb [--scale-mode NAME] [--filter nearest|linear]\n");
    return 2;
}
static int name_index(const char *values,const char *value) {
    int index=0;
    while (*values) {
        const char *end=strchr(values,'|');
        size_t n=end ? (size_t)(end-values) : strlen(values);
        if (strlen(value)==n && !memcmp(values,value,n)) return index;
        if (!end) break;
        values=end+1; ++index;
    }
    return -1;
}
int main(int argc,char **argv) {
    const char *in_path,*out_path=NULL,*label=NULL,*value="";
    double age=120;
    int width,height,scale=1,native_w=0,native_h=0,preset=0,scale_mode=HOST_SCALE_ASPECT,linear=1,i;
    unsigned out_w=0,out_h=0;
    gpu_scanout_t timing={0,0,0};
    uint8_t *rgb; FILE *f; size_t size;
    const uint8_t *out; int w,h,ok;
    gpu_t *g;
    if (argc<4) return usage();
    in_path=argv[1]; width=atoi(argv[2]); height=atoi(argv[3]);
    for (i=4;i<argc;++i) {
        if (!strcmp(argv[i],"--scale") && i+1<argc) scale=atoi(argv[++i]);
        else if (!strcmp(argv[i],"--native") && i+2<argc) { native_w=atoi(argv[i+1]); native_h=atoi(argv[i+2]); i+=2; }
        else if (!strcmp(argv[i],"--timing") && i+3<argc) {
            timing.horizontal=(uint32_t)strtoul(argv[i+1],NULL,0); timing.vertical=(uint32_t)strtoul(argv[i+2],NULL,0);
            timing.mode=(uint32_t)strtoul(argv[i+3],NULL,0); i+=3;
        }
        else if (!strcmp(argv[i],"--size") && i+1<argc) { if (sscanf(argv[++i],"%ux%u",&out_w,&out_h)!=2) return usage(); }
        else if (!strcmp(argv[i],"--crt") && i+1<argc) { if ((preset=name_index("off|pvm|tv|arcade",argv[++i]))<0) return usage(); }
        else if (!strcmp(argv[i],"--scale-mode") && i+1<argc) { if ((scale_mode=name_index("aspect|integer|stretch|aspect-crop",argv[++i]))<0) return usage(); }
        else if (!strcmp(argv[i],"--filter") && i+1<argc) { if ((linear=name_index("nearest|linear",argv[++i]))<0) return usage(); }
        else if (!strcmp(argv[i],"--out") && i+1<argc) out_path=argv[++i];
        else if (!strcmp(argv[i],"--osd-label") && i+1<argc) label=argv[++i];
        else if (!strcmp(argv[i],"--osd-value") && i+1<argc) value=argv[++i];
        else if (!strcmp(argv[i],"--osd-age") && i+1<argc) age=atof(argv[++i]);
        else return usage();
    }
    if (width<=0 || height<=0 || scale<1 || scale>8 || native_w<=0 || native_h<=0 || !out_w || !out_h || !out_path) return usage();
    size=(size_t)width*(size_t)height*3;
    rgb=malloc(size); if (!rgb) return 2;
    f=fopen(in_path,"rb");
    if (!f || fread(rgb,1,size,f)!=size) { fprintf(stderr,"cannot read %s\n",in_path); return 2; }
    fclose(f);
    g=gpu_create(); if (!g) return 2;
    if (!host_gpu_configure("vulkan",(unsigned)scale,0,1,NULL,NULL) || !host_gpu_start(g,NULL) || !host_gpu_enabled()) {
        fprintf(stderr,"Vulkan unavailable\n"); return 2;
    }
    host_gpu_presentation(scale_mode,linear);
    host_osd_configure(1,NULL); host_osd_clock(0);
    if (label) host_osd_post(label,value,0);
    host_osd_clock(age);
    ok=host_gpu_present_image(rgb,width,height,timing,native_w,native_h,out_w,out_h,preset,&out,&w,&h);
    if (ok) {
        f=fopen(out_path,"wb");
        ok=f && fwrite(out,1,(size_t)w*(size_t)h*3,f)==(size_t)w*(size_t)h*3;
        if (f && fclose(f)) ok=0;
    }
    printf("{\"width\":%d,\"height\":%d,\"preset\":%d,\"ok\":%d}\n",ok?w:0,ok?h:0,preset,ok);
    host_gpu_stop(); host_osd_shutdown(); free(g); free(rgb);
    return ok?0:1;
}
