#include "pgxp.h"
#include "gpu.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static unsigned triangles,hits;
static pgxp_vertex_t captured[2][3];
void __real_gpu_raster_triangle(gpu_raster_t *,gpu_vertex_t,gpu_vertex_t,gpu_vertex_t,gpu_paint_t);
void __wrap_gpu_raster_triangle(gpu_raster_t *s,gpu_vertex_t a,gpu_vertex_t b,gpu_vertex_t c,gpu_paint_t p) {
    pgxp_vertex_t v[3];
    if(pgxp_triangle(s,v)){memcpy(captured[triangles%2],v,sizeof(v));++hits;}
    ++triangles;__real_gpu_raster_triangle(s,a,b,c,p);
}
static uint32_t xy(int x,int y){return((uint32_t)x&65535)|((uint32_t)y<<16);}
static void record(int x,int y,unsigned z){pgxp_record(NULL,xy(x,y),(int64_t)x*65536+16384,(int64_t)y*65536+32768,z);}
static void seed(void){record(-10,-20,100);record(20,-20,200);record(-10,30,400);}
static void draw(gpu_t *g,int quad,unsigned expected){
    hits=triangles=0;gpu_gp0(g,quad?0x280000ff:0x200000ff);
    gpu_gp0(g,xy(-10,-20));gpu_gp0(g,xy(20,-20));gpu_gp0(g,xy(-10,30));
    if(quad)gpu_gp0(g,xy(20,30));
    assert(hits==expected);
}
int main(void){
    gpu_t *g=gpu_create(),*other=gpu_create();assert(g&&other);
    gpu_gp0(g,0xe3000000);gpu_gp0(g,0xe407ffff);
    gpu_gp0(g,0xe5000000|40|((unsigned)(-5)&2047)<<11);
    pgxp_bind(g,PGXP_VERTEX);seed();record(20,30,800);draw(g,1,2);
    assert(captured[0][0].x==29.75f&&captured[0][0].y==-25&&captured[0][0].w==1);
    assert(captured[1][0].x==59.75f&&captured[1][2].y==25);
    draw(other,0,0);
    pgxp_bind(g,PGXP_PERSPECTIVE);seed();draw(g,0,1);
    assert(captured[0][0].w==100&&captured[0][1].w==200&&captured[0][2].w==400);
    /* A later projection cannot change any already consumed endpoint this frame. */
    pgxp_record(NULL,xy(-10,-20),-10*INT64_C(65536)+60000,-20*INT64_C(65536)+100,101);
    draw(g,0,1);assert(captured[0][0].x==29.75f&&captured[0][0].w==100);
    pgxp_vblank(g);draw(g,0,1);
    assert(captured[0][0].x>30&&captured[0][0].x<=30.5f&&captured[0][0].w==101);
    /* Strict ambiguity affects only its vertex, never its neighbours' endpoints. */
    pgxp_bind(g,PGXP_VERTEX);pgxp_policy(PGXP_STRICT);seed();
    pgxp_record(NULL,xy(-10,-20),-10*INT64_C(65536)+12345,-20*INT64_C(65536)+32768,100);
    draw(g,0,1);assert(pgxp_stats().precise==0&&pgxp_stats().partial==1);
    assert(captured[0][0].x==30&&captured[0][1].x==59.75f);
    /* Missing fourth vertex keeps the shared endpoints precise on both halves. */
    pgxp_bind(g,PGXP_VERTEX);seed();draw(g,1,2);
    assert(captured[0][1].x==captured[1][0].x&&captured[0][2].y==captured[1][1].y);
    assert(captured[1][2].x==60&&captured[1][2].y==25);
    record(20,30,800);draw(g,1,2);assert(captured[1][2].x==60);
    pgxp_vblank(g);draw(g,1,2);assert(captured[1][2].x==59.75f);
    /* No common polygon generation is needed for independently valid endpoints. */
    pgxp_bind(g,PGXP_VERTEX);record(-10,-20,100);record(20,-20,200);
    pgxp_vblank(g);record(-10,30,400);draw(g,0,1);assert(pgxp_stats().precise==1);
    pgxp_bind(g,PGXP_PERSPECTIVE);seed();
    for(unsigned age=0;age<4;++age)pgxp_vblank(g);
    draw(g,0,1);assert(pgxp_stats().precise_age[4]==1);
    pgxp_vblank(g);draw(g,0,0);
    pgxp_bind(g,PGXP_PERSPECTIVE);seed();record(-10,-20,0);draw(g,0,1);
    assert(captured[0][0].x==30&&captured[0][0].w==1&&captured[0][1].w==1);
    pgxp_bind(g,PGXP_VERTEX);seed();
    gpu_gp0(g,0x600000ff);gpu_gp0(g,xy(-10,-20));gpu_gp0(g,xy(10,10));assert(pgxp_stats().polygons==0);
    pgxp_bind(g,PGXP_OFF);seed();draw(g,0,0);assert(pgxp_stats().projections==0);
    pgxp_bind(NULL,PGXP_OFF);free(g);free(other);puts("PGXP cache/parser contracts passed");return 0;
}
