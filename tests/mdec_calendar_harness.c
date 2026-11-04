#include "hw.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>

typedef struct { uint64_t cpu, device; uint32_t source; } irq_t;
static irq_t irqs[4096];
static unsigned irq_count;

void mdec_trace_observe(cpu_t *c, unsigned kind, uint32_t address, unsigned width, uint32_t value) {
    (void)width; (void)value;
    if (kind == 2) {
        assert(irq_count < sizeof(irqs) / sizeof(*irqs));
        irqs[irq_count].cpu = c->cycles;
        irqs[irq_count].device = c->hw->now;
        irqs[irq_count++].source = address;
    }
}
cpu_t *calendar_create(unsigned depth, unsigned layout, unsigned control) {
    cpu_t *c = calloc(1, sizeof(*c));
    unsigned i;
    static const spu_backend_t sink = {NULL, NULL};
    assert(c); cpu_init(c); hw_get(c); irq_count = 0;
    if (control) spu_set_backend(c->hw->spu, &sink, NULL);
    hw_write(c, 0x1f801da4, 2, 4);
    hw_write(c, 0x1f801dac, 2, 4);
    hw_write(c, 0x1f801daa, 2, 0xc0c0);
    hw_write(c, 0x1f801824, 4, 0xe0000000);
    hw_write(c, 0x1f801820, 4, 0x40000001);
    for (i = 0; i < 32; ++i) hw_write(c, 0x1f801820, 4, 0x10101010);
    hw_write(c, 0x1f801820, 4, 0x60000000);
    for (i = 0; i < 32; ++i) hw_write(c, 0x1f801820, 4, i < 4 ? 0x5a825a82 : 0);
    hw_write(c, 0x1f8010f0, 4, 0x88888);
    hw_write(c, 0x1f8010f4, 4, 0x9f0000);
    for (i = 0; i < 600; ++i) {
        uint32_t address = layout == 1 ? (RAM_SIZE - 128 + i * 4) & (RAM_SIZE - 1) :
                           layout == 2 ? 0x4000 - i * 4 : 0x2000 + i * 4;
        ram_write(c, address, 4, 0xfe000400 | (i % 6) * 7);
    }
    hw_write(c, 0x1f801820, 4, 0x20000000 | (depth << 27) | 600);
    hw_write(c, 0x1f801080, 4, layout == 1 ? RAM_SIZE - 128 : layout == 2 ? 0x4000 : 0x2000);
    hw_write(c, 0x1f801084, 4, 0x300c8);
    hw_write(c, 0x1f801088, 4, 0x01000201 | (layout == 2 ? 2 : 0));
    return c;
}
void calendar_advance(cpu_t *c, uint64_t cycles) { c->cycles = cycles; hw_sync(c); assert(!c->halted); }
void calendar_write(cpu_t *c, uint32_t address, unsigned width, uint32_t value) { hw_write(c,address,width,value); }
uint32_t calendar_read(cpu_t *c, uint32_t address, unsigned width) { return hw_read(c,address,width); }
void calendar_ram_write(cpu_t *c, uint32_t address, uint32_t value) { mem_write32(c,address,value); }
uint32_t calendar_ram_read(cpu_t *c, uint32_t address) { return mem_read32(c,address); }
uint64_t calendar_spans(cpu_t *c) { return c->hw->media_calendar.spans; }
int calendar_spu_compare(cpu_t *a, cpu_t *b) {
    return memcmp(spu_ram(a->hw->spu), spu_ram(b->hw->spu), 512u * 1024u);
}
unsigned calendar_irq_count(void) { return irq_count; }
uint64_t calendar_irq(unsigned index, unsigned field) {
    assert(index < irq_count);
    return field == 0 ? irqs[index].cpu : field == 1 ? irqs[index].device : irqs[index].source;
}
/* Identical layouts, separate -Bsymbolic libraries: compare actual bytes, not
 * hashes. Diagnostic batching counters and lazy SPU sample cursors are private. */
int calendar_compare(cpu_t *a, cpu_t *b) {
    struct hw *x = a->hw, *y = b->hw;
    if (a->cycles != b->cycles || a->mmio_epoch != b->mmio_epoch || x->now != y->now) return 1;
    if (memcmp(a->ram, b->ram, RAM_SIZE)) return 2;
    if (memcmp(&x->mdec, &y->mdec, sizeof(x->mdec))) return 3;
    if (memcmp(x->dma, y->dma, sizeof(x->dma))) return 4;
    if (memcmp(x->media_dma, y->media_dma, sizeof(x->media_dma))) return 5;
    if (memcmp(&x->spu_dma, &y->spu_dma, sizeof(x->spu_dma))) return 6;
    if (x->dicr != y->dicr || x->i_stat != y->i_stat || x->vblanks != y->vblanks) return 7;
    if (memcmp(x->irq_raised, y->irq_raised, sizeof(x->irq_raised))) return 8;
    return 0;
}
void calendar_destroy(cpu_t *c) { hw_destroy(c); free(c); }
