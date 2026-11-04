#include "../runtime/hw.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
static uint32_t display_status;
uint32_t gpu_status(gpu_t *gpu) { (void)gpu; return display_status; }
void hw_irq_raise(cpu_t *c, unsigned source) { c->hw->i_stat |= 1u << source; }
#include "../runtime/timers.c"

int main(void) {
    static const uint16_t starts[] = {0,1,0x7fff,0xfffe,0xffff};
    static const uint16_t targets[] = {0,1,0x8000,0xfffe,0xffff};
    static const unsigned lengths[] = {0,1,2,17,65534,65535,65536,65537,131075};
    cpu_t *c = calloc(1,sizeof(*c));
    unsigned a,b,n,i,clock,div;
    assert(c); c->hw = calloc(1,sizeof(*c->hw)); assert(c->hw);
    for (a=0;a<5;++a) for (b=0;b<5;++b) for(n=0;n<9;++n) {
        hw_timer_t *t=&c->hw->timers[0];
        uint16_t counter=starts[a],mode=0x400;
        for(i=0;i<lengths[n];++i) {
            ++counter;
            if(counter==targets[b]) mode|=0x800;
            if(counter==0xffff) mode|=0x1000;
        }
        memset(t,0,sizeof(*t));t->count=starts[a];t->target=targets[b];t->mode=0x400;
        count(c,0,lengths[n]);assert(t->count==counter && t->mode==mode);
    }
    /* Materializing before an IRQ boundary must preserve the absolute horizon,
     * including the rational GPU dot clock and the /8 fractional clock. */
    for(i=0;i<3;++i) for(clock=0;clock<4;++clock) for(div=0;div<5;++div) {
        hw_timer_t *t=&c->hw->timers[i];uint64_t due;
        memset(c->hw,0,sizeof(*c->hw));
        display_status=div==4 ? 1u<<16 : div<<17;
        t->mode=0x450|(clock<<8);t->count=17;t->target=1000;
        due=timers_next(c);if(due==HW_NEVER)continue;
        for(n=1;n<=13;++n) {
            c->hw->now=due*n/15;c->hw->timers_dirty=0;
            timers_advance(c,0);
            assert(!c->hw->timers_dirty && timers_next(c)==due);
        }
        c->hw->now=due;c->hw->timers_dirty=0;timers_advance(c,0);
        assert(c->hw->timers_dirty && (t->mode&0x800));
    }
    free(c->hw);free(c);puts("PASS timer spans and unchanged absolute IRQ horizons");return 0;
}
