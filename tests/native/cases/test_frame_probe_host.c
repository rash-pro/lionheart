
#include "hw.h"
#include <stdlib.h>
void cpu_init(cpu_t *c) { memset(c,0,sizeof(*c)); c->hw=calloc(1,sizeof(*c->hw)); }
void hw_destroy(cpu_t *c) { free(c->hw); c->hw=NULL; }
void host_present(gpu_t *g) { (void)g; }
