#define main replay_harness_main
#include "spu_control_harness.c"
#undef main

static void growing_dirty_region(void) {
    spu_t *a=spu_create(),*b=spu_create();
    assert(a && b);
    a->ram[0x2001]=b->ram[0x2001]=7;
    REG(a,4)=REG(b,4)=0x1000;
    REG(a,6)=REG(b,6)=REG(a,14)=REG(b,14)=0x400;
    spu_set_backend(b,&sink,NULL);
    spu_write(a,0x1aa,0xc000);spu_write(b,0x1aa,0xc000);
    spu_write(a,0x188,1);spu_write(b,0x188,1);
    spu_advance(a,183*768);spu_advance(b,183*768);
    assert(b->control->loops&1);
    /* Enabling a work area over the cached block revokes the disjoint proof. */
    spu_write(a,0x1a2,0x400);spu_write(b,0x1a2,0x400);
    spu_write(a,0x1ac,4);spu_write(b,0x1ac,4);
    spu_write(a,0x1aa,0xc080);spu_write(b,0x1aa,0xc080);
    spu_advance(a,366*768);spu_advance(b,366*768);
    assert(b->control->aliases);
    equal(a,b);spu_destroy(a);spu_destroy(b);
}

int main(void) {
    unsigned n,step;
    for(step=0;step<10;++step) {
        spu_t *a=spu_create(),*b=spu_create();
        assert(a && b);
        for(n=0;n<24;++n) {
            unsigned base=0x2000+n*16;
            memset(a->ram+base+2,0x11,14);memset(b->ram+base+2,0x11,14);
            a->ram[base+1]=b->ram[base+1]=7;
            REG(a,n*16+4)=REG(b,n*16+4)=0x1000;
            REG(a,n*16+6)=REG(b,n*16+6)=(uint16_t)(base/8);
            REG(a,n*16+14)=REG(b,n*16+14)=(uint16_t)(base/8);
        }
        spu_set_backend(b,&sink,NULL);
        spu_write(a,0x1aa,0xc000);spu_write(b,0x1aa,0xc000);
        spu_write(a,0x1a6,0x400);spu_write(b,0x1a6,0x400);
        spu_write(a,0x188,0xffff);spu_write(b,0x188,0xffff);
        spu_write(a,0x18a,0xff);spu_write(b,0x18a,0xff);
        spu_advance(a,183*768);spu_advance(b,183*768);
        spu_advance(a,366*768);spu_advance(b,366*768);
        assert(b->control->loops==0xffffff);
        /* Mutate only after warming the cache, without an intervening sync. */
        if(step==1){spu_write(a,4,0xffff);spu_write(b,4,0xffff);}
        if(step==2){spu_write(a,14,0x402);spu_write(b,14,0x402);}
        if(step==3){spu_dma_write(a,0x00000200);spu_dma_write(b,0x00000200);}
        if(step==4){spu_write(a,0x190,2);spu_write(b,0x190,2);}
        if(step==5){spu_write(a,0x188,1);spu_write(b,0x188,1);}
        if(step==6){spu_write(a,0x18c,1);spu_write(b,0x18c,1);}
        if(step==7){spu_write(a,0x1ac,4);spu_write(b,0x1ac,4);}
        if(step==8){spu_write(a,0x1aa,0x4000);spu_write(b,0x1aa,0x4000);}
        if(step==9){spu_write(a,14,0x401);spu_write(b,14,0x401);}
        spu_advance(a,549*768);spu_advance(b,549*768);
        assert(spu_read(a,14)==spu_read(b,14));
        equal(a,b);spu_destroy(a);spu_destroy(b);
    }
    growing_dirty_region();puts("PASS loop cache invalidation and terminal fetches");return 0;
}
