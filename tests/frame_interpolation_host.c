/* Synthetic guest instructions and pixels only; no disc or game data. */
#include "cpu.h"
#include "hw.h"
#include "display.h"
#include "vulkan_renderer.h"
#include "pgxp.h"
#include "frame_scene.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

static void step(cpu_t *c,uint32_t instruction) {
    mem_write32(c,0x10000,instruction); c->pc=0x80010000;
    interp_step(c); assert(!c->halted);
}
static void reg(cpu_t *c,unsigned r,uint32_t value) {
    step(c,15u<<26 | r<<16 | value>>16);
    step(c,13u<<26 | r<<21 | r<<16 | (value&65535));
}
static uint32_t xy(int x,int y) { return (uint16_t)x | (uint32_t)(uint16_t)y<<16; }
static void projection(cpu_t *c,unsigned pass) {
    reg(c,1,0x20000);
    mem_write32(c,0x20014,pass*8);
    for (unsigned i=0;i<8;++i) {
        step(c,35u<<26 | 1u<<21 | 3u<<16 | i*4); step(c,0);
        step(c,18u<<26 | 6u<<21 | 3u<<16 | i<<11);
    }
    for (unsigned i=0;i<6;++i) step(c,50u<<26 | 1u<<21 | i<<16 | (0x100+i*4));
    step(c,0x4a080030);
}
static void control(cpu_t *c,unsigned index,uint32_t value) {
    reg(c,3,value); step(c,18u<<26 | 6u<<21 | 3u<<16 | index<<11);
}
int main(int argc,char **argv) {
    assert(argc==4);
    int enabled=atoi(argv[1]), unknown=atoi(argv[3]);
    cpu_t *c=malloc(sizeof(*c)); assert(c);
    cpu_init(c); hw_get(c); c->interpreter_only=1;
    c->instruction_limit=c->stall_limit=0; c->next_event=UINT64_MAX/2;
    if (enabled) assert(frame_tags_enable(c));
    gpu_t *g=c->hw->gpu;
    assert(host_gpu_configure("vulkan",4,0,1,NULL,NULL));
    assert(host_gpu_start(g,NULL) && host_gpu_enabled());
    pgxp_bind(g,PGXP_PERSPECTIVE);
    host_gpu_interpolation(g,enabled ? (unknown>=3 ? FRAME_SCENE_FIXED_FIELD : FRAME_SCENE_BATTLE) : 0);
    for (unsigned i=0;i<8;++i) mem_write32(c,0x20000+i*4,i==0 || i==2 || i==4 ? 4096 : 0);
    uint32_t inputs[]={xy(-10,-10),128,xy(10,-10),128,xy(-10,10),128};
    for (unsigned i=0;i<6;++i) mem_write32(c,0x20100+i*4,inputs[i]);
    control(c,24,40u<<16); control(c,25,40u<<16); control(c,26,128);
    gpu_gp1(g,0x06000000 | 600 | (3160u<<12));
    gpu_gp1(g,0x07000000 | 16 | (256u<<10)); gpu_gp1(g,0x08000001); gpu_gp1(g,0x03000000);
    gpu_gp0(g,0xe3000000); gpu_gp0(g,0xe407ffff); gpu_gp0(g,0xe1000400);
    for (unsigned pass=0;pass<4;++pass) {
        unsigned x=pass%2 ? 0 : 512;
        gpu_gp0(g,0x02000000); gpu_gp0(g,xy((int)x,0)); gpu_gp0(g,xy(320,240));
        gpu_gp0(g,0xe5000000 | x);
        if (unknown>=3) {
            if (unknown==6) {
                gpu_gp0(g,0xa0000000); gpu_gp0(g,xy(640,256)); gpu_gp0(g,xy(2,1));
                gpu_gp0(g,0x00100010+pass);
                gpu_gp0(g,0xe100051a); gpu_gp0(g,0x65000000); gpu_gp0(g,xy(0,0));
                gpu_gp0(g,0); gpu_gp0(g,xy(256,200)); gpu_gp0(g,0xe1000400);
            } else {
                gpu_gp0(g,unknown==5 ? 0x60101010+pass*8 : 0x60101010);
                gpu_gp0(g,xy(unknown==4 || unknown==7 ? (int)pass*2 : 0,0)); gpu_gp0(g,xy(320,200));
                if (unknown==7) {
                    gpu_gp0(g,0x60202020); gpu_gp0(g,xy(100,100)); gpu_gp0(g,xy(16,16));
                }
            }
        }
        projection(c,pass);
        reg(c,4,0x1f801810); reg(c,5,0x200000f8);
        step(c,43u<<26 | 4u<<21 | 5u<<16);
        for (unsigned i=0;i<3;++i) {
            step(c,18u<<26 | 5u<<16 | (12+i)<<11); step(c,0);
            if (unknown==1 && !i) reg(c,5,c->r[5]);
            step(c,43u<<26 | 4u<<21 | 5u<<16);
        }
        gpu_gp1(g,0x05000000 | x);
        if (unknown==2) {
            gpu_gp0(g,0x62808080); gpu_gp0(g,xy(0,0)); gpu_gp0(g,xy(320,240));
        }
        gpu_gp0(g,0x6000f800); gpu_gp0(g,xy(0,80)); gpu_gp0(g,xy(8,8));
        for (unsigned phase=0;phase<4;++phase) {
            char path[4096];
            snprintf(path,sizeof(path),"%s/frame-%02u.png",argv[2],pass*4+phase);
            gpu_vblank(g); ++c->hw->vblanks;
            assert(host_display_scaled_dump(g,path));
            assert(host_gpu_frame(g));
        }
    }
    char path[4096]; snprintf(path,sizeof(path),"%s/guest.bin",argv[2]);
    FILE *f=fopen(path,"wb"); assert(f);
    assert(fwrite(c->r,sizeof(c->r),1,f)==1);
    assert(fwrite(c->gte_data,sizeof(c->gte_data),1,f)==1);
    assert(fwrite(c->gte_ctrl,sizeof(c->gte_ctrl),1,f)==1);
    assert(fwrite(c->ram,sizeof(c->ram),1,f)==1);
    assert(fwrite(c->scratch,sizeof(c->scratch),1,f)==1);
    assert(fwrite(c->mmio,sizeof(c->mmio),1,f)==1);
    assert(fwrite(gpu_vram(g),1024*512*2,1,f)==1); assert(!fclose(f));
    host_gpu_stop(); hw_destroy(c); free(c);
}
