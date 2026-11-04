#include "pgxp.h"
#include "gpu.h"
#include <stdlib.h>
static uint32_t xy(unsigned x,unsigned y) { return x | (y<<16); }
int main(void) {
    gpu_t *g=gpu_create();
    if (!g) return 2;
    pgxp_bind(g,PGXP_OFF);
    gpu_gp1(g,0x03000000);
    gpu_gp0(g,0xe3000000); gpu_gp0(g,0xe407ffff); gpu_gp0(g,0xe1000400);
    pgxp_record(NULL,xy(20,20),20*65536+16384,20*65536+32768,100);
    pgxp_record(NULL,xy(180,20),180*65536+16384,20*65536+32768,400);
    pgxp_record(NULL,xy(20,180),20*65536+16384,180*65536+32768,200);
    pgxp_record(NULL,xy(20,20),20*65536+49152,20*65536+32768,100);
    gpu_vblank(g);
    (void)gpu_status(g); /* A late capture must seed the previous generation. */
    gpu_gp0(g,0x200000ff); gpu_gp0(g,xy(20,20)); gpu_gp0(g,xy(180,20)); gpu_gp0(g,xy(20,180));
    gpu_vblank(g);
    free(g); return 0;
}
