#include "cpu.h"
#include "hw.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>

void *__real_calloc(size_t, size_t);
static unsigned diagnostic_allocations;
void *__wrap_calloc(size_t count, size_t size) {
    if (size == sizeof(uint32_t) && count >= 100000 && ++diagnostic_allocations > 2) return NULL;
    return __real_calloc(count, size);
}

int main(int argc, char **argv) {
    cpu_t *c = malloc(sizeof(*c));
    char error[256];
    FILE *summary;
    assert(argc == 3 && c);
    cpu_init(c);
    assert(cdrom_open(c, argv[1], error, sizeof(error)));
    assert(c->hw->cd.lba_reads && c->hw->cd.lba_dma);
    /* The old disc uses the remaining diagnostic budget, on the host.
     * A replacement must still work when only its optional statistics fail. */
    assert(cdrom_swap(c, argv[2], error, sizeof(error)));
    assert(c->hw->cd.shell_open && c->hw->cd.sectors == 100001);
    assert(!c->hw->cd.lba_reads && !c->hw->cd.lba_dma);
    c->hw->cd.shell_open = 0; c->hw->cd.lid_due = HW_NEVER;
    c->hw->cd.reading = 1; c->hw->cd.read_due = c->hw->now;
    cdrom_event(c);
    assert(c->hw->cd.sectors_read == 1);
    c->hw->cd.data_size = 4; c->hw->cd.data_pos = 0;
    memcpy(c->hw->cd.data, "\1\2\3\4", 4);
    assert(cdrom_dma(c) == 0x04030201 && c->hw->cd.bytes_dma == 4);
    summary = tmpfile(); assert(summary);
    hw_summary(c, summary); fclose(summary);
    hw_destroy(c); free(c);
    return 0;
}
