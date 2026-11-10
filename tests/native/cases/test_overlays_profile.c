
#include "hw.h"
#include <stdlib.h>
int main(void) {
    cpu_t *c = malloc(sizeof(*c));
    cpu_init(c); hw_get(c);
    mem_write32(c, 0x80098000, 0x03e00008); mem_write32(c, 0x80098004, 0);
    c->interpreter_only = 1; dispatch(c, 0x80098000);
    c_count_recompiled(c, 0x80098000, 3);
    cpu_stop(c, "profile done"); hw_destroy(c); free(c); return 0;
}
