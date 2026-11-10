
#include "hw.h"
#include <stdlib.h>
void host_present(gpu_t *g);
int main(void) {
    cpu_t *old=malloc(sizeof(*old)), *c=malloc(sizeof(*c));
    cpu_init(old); old->cycles=100; old->interp_instructions=50;
    host_present(NULL);
    cpu_init(c); hw_destroy(old); free(old);
    for (unsigned frame=1; frame<=3; ++frame) {
        c->hw->vblanks=frame; c->cycles+=10; c->interp_instructions+=5;
        host_present(NULL);
    }
    hw_destroy(c); free(c);
    volatile unsigned value=0;
    for (unsigned i=0;i<10000000;++i) value+=i; /* Exercise time after CPU teardown. */
    return value==1;
}
