#include "gpu.h"
#include <stddef.h>
#include <stdlib.h>

void test_gpu_free(gpu_t *g) { free(g); }
void test_gpu_feed(gpu_t *g, const uint32_t *words, size_t count) {
    size_t i;
    for (i = 0; i < count; ++i) gpu_gp0(g, words[i]);
}
uint16_t test_gpu_pixel(gpu_t *g, unsigned x, unsigned y) {
    return gpu_vram(g)[(y & 511) * 1024 + (x & 1023)];
}

#ifdef GPU_HARNESS_MAIN
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
static uint32_t random_word(uint32_t *seed) {
    *seed = *seed * 1664525u + 1013904223u;
    return *seed;
}
int main(void) {
    uint32_t seed = 0x505358u;
    unsigned i, j;
    gpu_t *g = gpu_create();
    assert(g);
    for (i = 0; i < 10000; ++i) {
        gpu_gp1(g, 0x01000000);
        gpu_gp0(g, 0xe3000000);
        gpu_gp0(g, 0xe407ffff);
        for (j = 0; j < 16; ++j) gpu_gp0(g, random_word(&seed));
        gpu_gp1(g, 0x10000000 | (i & 15));
        (void)gpu_read(g);
        (void)gpu_status(g);
        gpu_vblank(g);
    }
    gpu_gp1(g, 0);
    assert(gpu_status(g) == 0x14802000);
    free(g);
    puts("GPU packet/reset stress: 10000 streams passed");
    return 0;
}
#endif
