#include "cpu.h"
#include "hw.h"
#include "gte.h"
#include <stdlib.h>
int main(void) {
    cpu_t *c=calloc(1,sizeof(*c));struct hw *hw=calloc(1,sizeof(*hw));if(!c||!hw)return 2;c->hw=hw;
    for(hw->vblanks=0;hw->vblanks<4;++hw->vblanks)
#ifdef FF8_GTE_SPECIALIZED_TRACE
        gte_nclip(c);
#else
        gte_cmd(c,6);
#endif
    free(hw);free(c);return 0;
}
