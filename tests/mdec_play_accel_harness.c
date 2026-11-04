#define main exact_calendar_main
#include "mdec_accel_harness.c"
#undef main

static uint64_t finish(cpu_t *c) {
    unsigned i;
    hw_write(c, 0x1f801090, 4, 0x10000);
    hw_write(c, 0x1f801094, 4, 0x10000 | (output_depth == 2 ? 3840 : 2560));
    hw_write(c, 0x1f801098, 4, 0x01000200);
    for (i = 0; i < 30000; ++i) {
        c->cycles += 7; hw_sync(c);
        if (c->hw->dma[0].completions && c->hw->dma[1].completions) return c->cycles;
    }
    assert(!"MDEC DMA did not complete");
    return 0;
}

int main(void) {
    unsigned mutate;
    for (output_depth = 2; output_depth < 4; ++output_depth) for (mutate = 0; mutate < 8; ++mutate) {
        cpu_t *a, *b;
        uint64_t scalar_end, preview_end;
        complex_input = mutate >= 3;
        a = create(0); b = create(1);
        if (mutate && mutate != 3 && mutate != 7) {
            unsigned word = mutate == 1 || mutate == 4 ? 0 : mutate == 5 ? 1 : mutate == 6 ? 3 : 5;
            unsigned address = 0x2000 + word * 4;
            uint32_t value = mutate >= 4 ? (ram_read(a,address,4) & 65535) | (0x110u << 16) : 0xfe000408;
            ram_write(a,address,4,value); ram_write(b,address,4,value);
        }
        if (mutate == 7) b->hw->mdec.accel->cancel(&b->hw->mdec);
        scalar_end = finish(a); preview_end = finish(b);
        if (!mutate) assert(preview_end < scalar_end);
        /* Timing may differ in play; completed DMA data, counts and IRQs may not. */
        a->cycles = b->cycles = scalar_end > preview_end ? scalar_end : preview_end;
        hw_sync(a); hw_sync(b); compare(a,b);
        assert(a->hw->dma[0].completions == 1 && a->hw->dma[1].completions == 1);
        assert((a->hw->dicr & 0x3000000) == 0x3000000 && (a->hw->i_stat & 8));
        mdec_control(&a->hw->mdec, 0xe0000000); mdec_control(&b->hw->mdec, 0xe0000000);
        compare(a,b);
        hw_destroy(a); hw_destroy(b); free(a); free(b);
    }
    assert(pixel_calls >= 80 && cancel_calls);
    return 0;
}
