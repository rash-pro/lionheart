#define _POSIX_C_SOURCE 200809L
/* Private FF8GPUR1/2 stream reader. Parser snapshots remain local-build ABI data. */
#include "display.h"
#include "vulkan_renderer.h"
#include "gpu_stream.h"
#include "../runtime/gpu.c"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <math.h>
#define main demo_unused_main
#include "gpu_demo.c"
#undef main

static unsigned errors,frames,reads,statuses,raw_frames;
static unsigned long long raw_pixels,commands;
static int scale, reference_mode, benchmark;
static uint16_t downloaded[1024*512];
static const char *shot, *frame_dir;
static int pgxp_enabled;
static FILE *vertex_trace;
#ifdef FF8_PGXP
static gpu_t *trace_gpu;
static void trace_vertex(void *ctx,uint64_t frame,uint32_t key,pgxp_vertex_t v,unsigned valid,unsigned ambiguous,unsigned age) {
    (void)ctx;
    fprintf(vertex_trace,"%llu,%u,%.9g,%.9g,%.9g,%u,%u,%u,%.9g,%.9g\n",(unsigned long long)frame,key,v.x,v.y,v.w,valid,ambiguous,age,
            v.x+trace_gpu->offset_x-trace_gpu->raster.left,v.y+trace_gpu->offset_y-trace_gpu->raster.top);
}
#endif
static uint64_t scaled_abs16,scaled_sq16,scaled_channels;
static unsigned scaled_frames,scaled_max16,shape_checks;
static double scaled_worst_mean;
static int compare_scaled(gpu_t *soft,gpu_t *vk) {
    static uint8_t expected[1024*512*3];
    const uint8_t *rgb; int w,h,sw,sh;
    if (!host_display_rgb(soft,expected,sizeof(expected),&w,&h) ||
        !host_gpu_rgb(vk,&rgb,&sw,&sh) || sw!=w*4 || sh!=h*4) return 0;
    uint64_t sum=0,square=0;
    for (int y=0;y<h;++y) for (int x=0;x<w;++x) for (int c=0;c<3;++c) {
        int box=0;
        for (int dy=0;dy<4;++dy) for (int dx=0;dx<4;++dx) box+=rgb[((y*4+dy)*sw+x*4+dx)*3+c];
        unsigned error=(unsigned)abs(box-expected[(y*w+x)*3+c]*16);
        sum+=error; square+=(uint64_t)error*error;
        if (error>scaled_max16) scaled_max16=error;
    }
    uint64_t channels=(uint64_t)w*h*3;
    double mean=channels?(double)sum/(16*channels):0;
    double rms=channels?sqrt((double)square/(256*channels)):0;
    scaled_abs16+=sum; scaled_sq16+=square; scaled_channels+=channels; ++scaled_frames;
    if (mean>scaled_worst_mean) scaled_worst_mean=mean;
    if (mean>12 || rms>48) { fprintf(stderr,"4x box error frame=%u mean=%.3f rms=%.3f max=%.3f\n",frames,mean,rms,scaled_max16/16.0); return 0; }
    return 1;
}
static void scaled_contracts(gpu_t *soft,gpu_t *vk) {
    gpu_t *both[]={soft,vk};
    for (unsigned i=0;i<2;++i) {
        gpu_t *g=both[i];
        gpu_gp1(g,0x05000000|20|(20u<<10)); gpu_gp1(g,0x06000000|512|(552u<<12));
        gpu_gp1(g,0x07000000|16|(18u<<10)); gpu_gp1(g,0x08000000); gpu_gp1(g,0x03000000);
        gpu_gp0(g,0xe1000400); rectangle(g,20,20,4,2,0,0);
        gpu_gp0(g,0x200000f8); gpu_gp0(g,xy(20,20)); gpu_gp0(g,xy(24,20)); gpu_gp0(g,xy(20,24));
    }
    const uint8_t *rgb; int w,h;
    uint8_t gap[48];
    if (!host_gpu_rgb(vk,&rgb,&w,&h) || w!=16 || h!=8) { ++errors; return; }
    unsigned low=255,high=0;
    for (int y=0;y<4;++y) for (int x=0;x<4;++x) {
        const uint8_t *src=rgb+((4+y)*w+8+x)*3;
        memcpy(gap+(y*4+x)*3,src,3);
        if (src[0]<low) low=src[0];
        if (src[0]>high) high=src[0];
    }
    if (low==high) { fprintf(stderr,"triangle lost 4x edge coverage\n"); ++errors; }
    // Two disjoint one-pixel uploads must preserve the scaled triangle between them.
    for (unsigned i=0;i<2;++i) for (unsigned x=20;x<=23;x+=3) {
        gpu_gp0(both[i],0xa0000000); gpu_gp0(both[i],xy((int)x,21)); gpu_gp0(both[i],xy(1,1)); gpu_gp0(both[i],0x7fff);
    }
    if (!host_gpu_rgb(vk,&rgb,&w,&h)) { ++errors; return; }
    for (int y=0;y<4;++y) for (int x=0;x<4;++x)
        if (memcmp(gap+(y*4+x)*3,rgb+((4+y)*w+8+x)*3,3)) { fprintf(stderr,"sparse upload overwrote scaled gap\n"); ++errors; y=4; break; }
    for (int native_x=0;native_x<=3;native_x+=3)
        for (int y=0;y<4;++y) for (int x=0;x<4;++x) for (int c=0;c<3;++c)
            if (rgb[((4+y)*w+native_x*4+x)*3+c]!=255) { fprintf(stderr,"sparse upload pixel not updated\n"); ++errors; y=4; break; }
    ++shape_checks;
    for (unsigned i=0;i<2;++i) {
        rectangle(both[i],20,20,4,2,0,0);
        gpu_gp0(both[i],0x4000f800); gpu_gp0(both[i],xy(20,20)); gpu_gp0(both[i],xy(23,21));
    }
    if (!host_gpu_rgb(vk,&rgb,&w,&h)) { ++errors; return; }
    unsigned mixed=0;
    for (int y=0;y<2;++y) for (int x=0;x<4;++x) {
        low=255; high=0;
        for (int dy=0;dy<4;++dy) for (int dx=0;dx<4;++dx) {
            unsigned v=rgb[((y*4+dy)*w+x*4+dx)*3+1];
            if (v<low) low=v;
            if (v>high) high=v;
        }
        if (low!=high) ++mixed;
    }
    if (!mixed) { fprintf(stderr,"line lost 4x edge coverage\n"); ++errors; }
    ++shape_checks;
}

static int compare(gpu_t *soft,gpu_t *vk) {
    unsigned i,bad=0;
    if (memcmp(gpu_vram(soft),gpu_vram(vk),sizeof(downloaded))) return 0;
    if (!host_gpu_download(vk,downloaded,0) || memcmp(gpu_vram(soft),downloaded,sizeof(downloaded))) return 0;
    if (!host_gpu_download(vk,downloaded,1)) return 0;
    for (i=0;i<1024*512;++i) if (downloaded[i]!=gpu_vram(soft)[i]) {
        if (!bad && reference_mode) fprintf(stderr,"native mismatch x=%u y=%u soft=%04x vk=%04x\n",i%1024,i/1024,gpu_vram(soft)[i],downloaded[i]);
        ++bad;
    }
    raw_pixels+=bad;
    if (bad) { ++raw_frames; if (scale==1 && raw_frames<4) fprintf(stderr,"raw VRAM frame=%u pixels=%u\n",frames,bad); }
    return !reference_mode || !bad;
}
static int display_matches(gpu_t *soft,gpu_t *vk,int tolerance) {
    uint8_t expected[4*8*3];
    const uint8_t *actual;
    int w,h,sw,sh;
    if (!host_display_rgb(soft,expected,sizeof(expected),&w,&h) ||
        !host_gpu_rgb(vk,&actual,&sw,&sh) || sw!=w*scale || sh!=h*scale) return 0;
    for (int y=0;y<sh;++y) for (int x=0;x<sw;++x) for (int c=0;c<3;++c) {
        int a=actual[(y*sw+x)*3+c], b=expected[((y/scale)*w+x/scale)*3+c];
        if (abs(a-b)>tolerance) { fprintf(stderr,"display mismatch x=%d y=%d channel=%d expected=%d actual=%d\n",x,y,c,b,a); return 0; }
    }
    return 1;
}
static void display_contracts(gpu_t *soft,gpu_t *vk,int true_color) {
    gpu_t *both[]={soft,vk};
    for (unsigned n=0;n<2;++n) {
        gpu_t *g=both[n];
        gpu_gp0(g,0xa0000000); gpu_gp0(g,xy(1023,511)); gpu_gp0(g,xy(4,2));
        gpu_gp0(g,0x12347c1f); gpu_gp0(g,0x03e07777); gpu_gp0(g,0x4321001f); gpu_gp0(g,0x7c005555);
        gpu_gp1(g,0x05000000|1023|(511u<<10));
        gpu_gp1(g,0x06000000|512|(552u<<12));
        gpu_gp1(g,0x07000000|16|(18u<<10));
        gpu_gp1(g,0x08000010); gpu_gp1(g,0x03000000);
    }
    if (!display_matches(soft,vk,0)) ++errors; // packed 24-bit, byte and row wrap
    /* True colour retains 0xf8 from 8-bit paint; the reference expands 5-bit white to 0xff. */
    gpu_gp1(soft,0x08000000); gpu_gp1(vk,0x08000000);
    if (!display_matches(soft,vk,true_color?7:0)) ++errors; // 15-bit scanout
    for (unsigned n=0;n<2;++n) {
        gpu_t *g=both[n];
        gpu_gp0(g,0xe3000000); gpu_gp0(g,0xe407ffff);
        gpu_gp1(g,0x05000000|20|(20u<<10));
        gpu_gp0(g,0xa0000000); gpu_gp0(g,xy(512,300)); gpu_gp0(g,xy(8,1));
        gpu_gp0(g,0x001f0000); gpu_gp0(g,0x7c0003e0); gpu_gp0(g,0x11117fff); gpu_gp0(g,0x33332222);
        for (unsigned row=0;row<2;++row) {
            gpu_gp0(g,0xa0000000); gpu_gp0(g,xy(640,row?511:256)); gpu_gp0(g,xy(128,1));
            for (unsigned i=0;i<64;++i) gpu_gp0(g,row?0x01040302:0x04030201);
        }
        gpu_gp0(g,0xe100049a); // 8-bit texture, draw into the display area
        gpu_gp0(g,0x65000000); gpu_gp0(g,xy(20,20)); gpu_gp0(g,((300u*64+32)<<16)|254); gpu_gp0(g,xy(4,1));
    }
    if (!display_matches(soft,vk,true_color?7:0)) ++errors; // sprite U crosses 255 -> 0
    for (unsigned n=0;n<2;++n) {
        gpu_t *g=both[n];
        gpu_gp0(g,0xe100349a); // horizontal and vertical sprite flips
        gpu_gp0(g,0x65000000); gpu_gp0(g,xy(20,20)); gpu_gp0(g,(300u*64+32)<<16); gpu_gp0(g,xy(4,2));
    }
    if (!display_matches(soft,vk,true_color?7:0)) ++errors;
    for (unsigned n=0;n<2;++n) {
        gpu_t *g=both[n];
        gpu_gp0(g,0xa0000000); gpu_gp0(g,xy(640,256)); gpu_gp0(g,xy(16,1));
        for (unsigned i=0;i<4;++i) gpu_gp0(g,0x001f001f);
        for (unsigned i=0;i<4;++i) gpu_gp0(g,0x03e003e0);
        gpu_gp0(g,0xe100051a); gpu_gp0(g,0xe2000401); // direct colour, window forces U bit 3
        gpu_gp0(g,0x65000000); gpu_gp0(g,xy(20,20)); gpu_gp0(g,0); gpu_gp0(g,xy(4,1));
    }
    if (!display_matches(soft,vk,true_color?7:0)) ++errors;
    for (unsigned n=0;n<2;++n) {
        gpu_t *g=both[n];
        gpu_gp0(g,0xe3000000); gpu_gp0(g,0xe407ffff);
        gpu_gp0(g,0xe1000000); // display-area drawing disabled, interlace field exclusion
        gpu_gp1(g,0x05000000|20|(20u<<10)); gpu_gp1(g,0x08000024);
        rectangle(g,20,20,4,4,0x00f800,0);
    }
    if (!display_matches(soft,vk,true_color?7:0)) ++errors;
    for (unsigned n=0;n<2;++n) { gpu_vblank(both[n]); rectangle(both[n],20,20,4,4,0xf80000,0); }
    if (!display_matches(soft,vk,true_color?7:0)) ++errors;
    if (scale==4 && !true_color && !reference_mode) scaled_contracts(soft,vk);
    gpu_gp1(soft,0x03000001); gpu_gp1(vk,0x03000001);
    if (!display_matches(soft,vk,0)) ++errors; // display disabled is black
    memset(soft,0,sizeof(*soft)); memset(vk,0,sizeof(*vk)); reset(soft); reset(vk);
    if (!host_gpu_resume(vk)) ++errors;
}
static int replay_main(int argc,char **argv) {
    FILE *f=NULL;
    gpu_t *soft=gpu_create(),*vk=gpu_create();
    char magic[8];
    uint32_t words[2];
    uint8_t row[8];
    size_t bytes=0;
    int wsi=argc>1 && !strcmp(argv[1],"--self-test-wsi");
    int shutdown=argc>1 && !strcmp(argv[1],"--self-test-shutdown");
    int asynchronous=argc>1 && !strcmp(argv[1],"--self-test-async");
    int fallback_window=argc>1 && !strcmp(argv[1],"--self-test-fallback-window");
    int fallback=fallback_window || (argc>1 && !strcmp(argv[1],"--self-test-fallback"));
    int synthetic=argc>1 && (!strcmp(argv[1],"--self-test") || wsi || shutdown || asynchronous);
    struct timespec start,end;
    if (argc<3 || !(scale=atoi(argv[2])) || !soft || !vk) return 2;
    int strict_policy=0; const char *trace_path=NULL;
    for (int i=7;i<argc;++i) {
        if (!strcmp(argv[i],"--pgxp-policy") && i+1<argc) {
            const char *value=argv[++i];
            if (!strcmp(value,"strict")) strict_policy=1;
            else if (strcmp(value,"latest")) return 2;
        } else if (!strcmp(argv[i],"--vertex-trace") && i+1<argc) trace_path=argv[++i];
        else if (!strcmp(argv[i],"--benchmark")) benchmark=1;
        else if (!strcmp(argv[i],"--dump-scaled-frames") && i+1<argc) frame_dir=argv[++i];
        else if (!strcmp(argv[i],"--pgxp") && i+1<argc) {
            const char *value=argv[++i];
            if (!strcmp(value,"vertex")) pgxp_enabled=1;
            else if (!strcmp(value,"perspective")) pgxp_enabled=2;
            else if (strcmp(value,"off")) return 2;
        } else return 2;
    }
#ifdef FF8_PGXP
    pgxp_bind(vk,(pgxp_mode_t)pgxp_enabled);
    pgxp_policy(strict_policy?PGXP_STRICT:PGXP_LATEST);
    if (trace_path) {
        trace_gpu=vk; vertex_trace=fopen(trace_path,"w"); if (!vertex_trace) return 2;
        fprintf(vertex_trace,"frame,key,x,y,z,precise,ambiguous,age,screen_x,screen_y\n");
        pgxp_resolved_sink(trace_vertex,NULL);
    }
#else
    if (pgxp_enabled || strict_policy || trace_path) return 2;
#endif
    reference_mode=!pgxp_enabled && scale==1 && !(argc>5?atoi(argv[5]):0) && (argc>6?atoi(argv[6]):1)==1;
    shot=argc>3 ? argv[3] : NULL;
    if (!host_gpu_configure("vulkan",(unsigned)scale,argc>5?atoi(argv[5]):0,argc>6?(unsigned)atoi(argv[6]):1,NULL,argc>4?argv[4]:NULL)) return 2;
    if ((wsi || fallback_window) ? (!host_display_init(0,NULL,NULL) || !host_display_gpu(vk)) : !host_gpu_start(vk,NULL)) return 2;
    if (fallback) {
        if (host_gpu_enabled()) return 1;
        scene(soft); scene(vk);
        int ok=!memcmp(gpu_vram(soft),gpu_vram(vk),sizeof(downloaded)) && gpu_status(soft)==gpu_status(vk);
        if (shot) ok=ok && host_display_dump(vk,shot);
        if (fallback_window) { host_present(vk); ok=ok && host_display_running() && host_display_shutdown(vk); }
        free(soft); free(vk);
        printf("{\"fallback\":\"%s\"}\n",ok?"pass":"fail"); return ok?0:1;
    }
#ifdef FF8_VULKAN_TESTING
    if (argc>1 && !strcmp(argv[1],"--self-test-fault")) {
        unsigned phase=(unsigned)scale;
        if (!host_gpu_enabled()) return 1;
        if (phase==1) host_gpu_test_fault(phase);
        scene(soft); scene(vk);
        if (phase==2 || phase==5) {
            host_gpu_test_fault(phase);
            for (int i=0;i<4 && host_gpu_enabled();++i) host_gpu_frame(vk);
        } else if (phase>=3) {
            const uint8_t *pixels; int w,h;
            host_gpu_test_fault(phase); (void)host_gpu_rgb(vk,&pixels,&w,&h);
        }
        int ok=!host_gpu_enabled() && !memcmp(gpu_vram(soft),gpu_vram(vk),sizeof(downloaded));
        if (phase==3) ok=ok && host_gpu_test_maps()==0;
        host_gpu_stop(); free(soft); free(vk);
        printf("{\"fault\":%u,\"fallback\":\"%s\"}\n",phase,ok?"pass":"fail"); return ok?0:1;
    }
#endif
    clock_gettime(CLOCK_MONOTONIC,&start);
    if (synthetic) {
        display_contracts(soft,vk,argc>5?atoi(argv[5]):0);
        scene(soft); scene(vk);
        if (shutdown) { host_gpu_stop(); free(soft); free(vk); puts("{\"shutdown\":\"pass\"}"); return 0; }
        for (unsigned n=0;n<8;++n) {
            /* Transfers: wrap, mask preserve/force, horizontal and vertical feedback. */
            gpu_t *both[]={soft,vk};
            for (unsigned g=0;g<2;++g) {
                gpu_gp0(both[g],0xe6000001);
                gpu_gp0(both[g],0xa0000000); gpu_gp0(both[g],xy(1023,511)); gpu_gp0(both[g],xy(3,2));
                gpu_gp0(both[g],0x7fff1234); gpu_gp0(both[g],0x801f03e0); gpu_gp0(both[g],0x7c003def);
                gpu_gp0(both[g],0xe6000002);
                gpu_gp0(both[g],0x80000000); gpu_gp0(both[g],xy(1023,511)); gpu_gp0(both[g],xy(0,0)); gpu_gp0(both[g],xy(3,2));
                gpu_gp0(both[g],0xe6000000);
                gpu_gp0(both[g],0xc0000000); gpu_gp0(both[g],xy(1023,511)); gpu_gp0(both[g],xy(3,2));
            }
            for (unsigned i=0;i<3;++i) { ++reads; if (gpu_read(soft)!=gpu_read(vk)) ++errors; }
            if (!asynchronous && !compare(soft,vk)) ++errors;
            if (asynchronous && !host_gpu_scanout(vk)) ++errors;
            gpu_vblank(soft); gpu_vblank(vk); ++frames;
            host_gpu_frame(vk);
        }
    } else {
        f=fopen(argv[1],"rb");
        if (!f || fread(magic,1,8,f)!=8 ||
            (memcmp(magic,"FF8GPUR1",8) && memcmp(magic,"FF8GPUR2",8))) return 2;
        int version2=magic[7]=='2';
        while ((bytes=fread(row,1,sizeof(row),f))==sizeof(row)) {
            words[0]=gpu_stream_le32(row); words[1]=gpu_stream_le32(row+4);
            uint32_t value=words[1],a=value,b=value;
            switch (words[0]) {
            case 0: if (!benchmark) gpu_gp0(soft,value); gpu_gp0(vk,value); break;
            case 1: if (!benchmark) gpu_gp1(soft,value); gpu_gp1(vk,value); break;
            case 2:
                if (!benchmark) gpu_vblank(soft);
                gpu_vblank(vk); ++frames;
                if (benchmark) {
                    if (!host_gpu_scanout(vk)) ++errors;
                } else {
                    if (!compare(soft,vk)) ++errors;
                    if (scale==4 && !pgxp_enabled && !compare_scaled(soft,vk)) ++errors;
                }
                if (frame_dir) {
                    char path[4096];
                    if (snprintf(path,sizeof(path),"%s/frame-%06u.png",frame_dir,frames)>= (int)sizeof(path) ||
                        !host_display_scaled_dump(vk,path)) ++errors;
                }
                host_gpu_frame(vk); break;
            case 3: if (!benchmark) a=gpu_read(soft); b=gpu_read(vk); ++reads; break;
            case 4: if (!benchmark) a=gpu_status(soft); b=gpu_status(vk); ++statuses; break;
            case 5:
                if (value!=sizeof(*soft) || fread(soft,sizeof(*soft),1,f)!=1) return 2;
                memcpy(vk,soft,sizeof(*soft)); if (!host_gpu_resume(vk)) return 2; break;
            case 6: break; /* absolute VBlank origin, not a new frame */
            case 7: {
                uint8_t payload[20];
                if (!version2 || fread(payload,sizeof(payload),1,f)!=1) return 2;
#ifdef FF8_PGXP
                int64_t x=(int64_t)(int32_t)gpu_stream_le32(payload+4)*INT64_C(4294967296)+gpu_stream_le32(payload);
                int64_t y=(int64_t)(int32_t)gpu_stream_le32(payload+12)*INT64_C(4294967296)+gpu_stream_le32(payload+8);
                pgxp_record(NULL,value,x,y,gpu_stream_le32(payload+16));
#endif
                break;
            }
            case 9:
                if (!version2) return 2;
#ifdef FF8_PGXP
                if (!pgxp_mark_ambiguous(value)) return 2;
#endif
                break;
            case 8:
                if (!version2 || value) return 2;
#ifdef FF8_PGXP
                pgxp_vblank(vk);
#endif
                break;
            default: return 2;
            }
            if (a!=value || b!=value) { if (errors++<4) fprintf(stderr,"read/status mismatch record=%llu kind=%u\n",commands,words[0]); }
            ++commands;
        }
        if (bytes || ferror(f) || fclose(f)) return 2;
    }
    if (!benchmark && !compare(soft,vk)) ++errors;
    if (shot && !host_display_scaled_dump(vk,shot)) ++errors;
    if (wsi) { host_present(vk); if (!host_display_running()) ++errors; }
    host_gpu_frame(vk);
    clock_gettime(CLOCK_MONOTONIC,&end);
    double seconds=(double)(end.tv_sec-start.tv_sec)+(double)(end.tv_nsec-start.tv_nsec)*1e-9;
#ifdef FF8_PGXP
    pgxp_stats_t stats=pgxp_stats();
    printf("{\"pgxp\":{\"mode\":%d,\"projections\":%llu,\"polygons\":%llu,\"candidates\":%llu,\"precise\":%llu,\"ambiguous\":%llu,\"misses\":%llu,\"generation_misses\":%llu,\"collisions\":%llu,\"rejected\":%llu,\"overflow\":%llu,\"partial\":%llu,\"vertex_precise\":%llu,\"vertex_lookups\":%llu},",
        pgxp_enabled,(unsigned long long)stats.projections,(unsigned long long)stats.polygons,
        (unsigned long long)stats.candidates,(unsigned long long)stats.precise,(unsigned long long)stats.ambiguous,
        (unsigned long long)stats.misses,(unsigned long long)stats.generation_misses,
        (unsigned long long)stats.collisions,(unsigned long long)stats.rejected,(unsigned long long)stats.overflow,(unsigned long long)stats.partial,(unsigned long long)stats.vertex_precise,(unsigned long long)stats.vertex_lookups);
#else
    printf("{");
#endif
#ifdef FF8_PGXP
    printf("\"precise_age\":[");
    for (unsigned age=0;age<PGXP_GENERATIONS;++age)
        printf("%s%llu",age?",":"",(unsigned long long)stats.precise_age[age]);
    printf("],");
#endif
    printf("\"scaled_frames\":%u,\"scaled_mean_abs_error\":%.6f,\"scaled_rms_error\":%.6f,\"scaled_max_error\":%.6f,\"scaled_worst_frame_mean\":%.6f,\"shape_checks\":%u,",
        scaled_frames,scaled_channels?(double)scaled_abs16/(16*scaled_channels):0,
        scaled_channels?sqrt((double)scaled_sq16/(256*scaled_channels)):0,scaled_max16/16.0,scaled_worst_mean,shape_checks);
    printf("\"frames\":%u,\"commands\":%llu,\"reads\":%u,\"statuses\":%u,\"errors\":%u,\"raw_mismatch_frames\":%u,\"raw_mismatch_pixels\":%llu,\"wall_seconds\":%.6f}\n",
           frames,commands,reads,statuses,errors,raw_frames,raw_pixels,seconds);
    if (wsi) { if (!host_display_shutdown(vk)) ++errors; } else host_gpu_stop();
    free(soft); free(vk);
    return errors?1:0;
}

int main(int argc,char **argv) {
    int result=replay_main(argc,argv);
    /* Parser errors also drain Vulkan before the driver's LLVM exit handlers. */
    host_gpu_stop();
    if (vertex_trace && fclose(vertex_trace)) return 2;
    return result;
}
