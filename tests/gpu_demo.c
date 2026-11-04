#include "gpu.h"
#include "display.h"
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

static uint32_t xy(int x, int y) { return ((uint32_t)y & 65535) << 16 | ((uint32_t)x & 65535); }
static void rectangle(gpu_t *g, int x, int y, int w, int h, uint32_t color, int semi) {
    gpu_gp0(g, (semi ? 0x62000000 : 0x60000000) | color);
    gpu_gp0(g, xy(x, y)); gpu_gp0(g, xy(w, h));
}

static void scene(gpu_t *g) {
    unsigned x, y, i;
    gpu_gp0(g, 0xe3000000); gpu_gp0(g, 0xe4000000 | 319 | (239 << 10));
    gpu_gp0(g, 0xe1000600);
    rectangle(g, 0, 0, 320, 240, 0x281810, 0);
    for (y = 0; y < 240; y += 16) for (x = 0; x < 320; x += 16)
        if ((x / 16 + y / 16) & 1) rectangle(g, (int)x, (int)y, 16, 16, 0x302018, 0);
    /* Flat quad, dithered Gouraud triangle, Gouraud quad. */
    gpu_gp0(g, 0x2800b8f0);
    gpu_gp0(g, xy(12, 16)); gpu_gp0(g, xy(90, 16)); gpu_gp0(g, xy(12, 72)); gpu_gp0(g, xy(90, 72));
    gpu_gp0(g, 0x300000ff); gpu_gp0(g, xy(108, 16));
    gpu_gp0(g, 0x0000ff00); gpu_gp0(g, xy(190, 16));
    gpu_gp0(g, 0x00ff0000); gpu_gp0(g, xy(108, 78));
    gpu_gp0(g, 0x38ffffff); gpu_gp0(g, xy(214, 16));
    gpu_gp0(g, 0x00ffff00); gpu_gp0(g, xy(304, 16));
    gpu_gp0(g, 0x00ff0080); gpu_gp0(g, xy(214, 76));
    gpu_gp0(g, 0x00000000); gpu_gp0(g, xy(304, 76));
    /* 4-bit checker texture and CLUT outside the display buffer. */
    gpu_gp0(g, 0xa0000000); gpu_gp0(g, xy(640, 256)); gpu_gp0(g, xy(8, 32));
    for (y = 0; y < 32; ++y) for (x = 0; x < 8; x += 2) {
        uint32_t a = ((x / 2 + y / 8) & 1) ? 0x1111 : 0x2222;
        gpu_gp0(g, a | (a << 16));
    }
    gpu_gp0(g, 0xa0000000); gpu_gp0(g, xy(512, 300)); gpu_gp0(g, xy(4, 1));
    gpu_gp0(g, 0x03ff0000); gpu_gp0(g, 0x00007c1f);
    gpu_gp0(g, 0xe100061a);
    gpu_gp0(g, 0xe2000000 | 28 | (28 << 5));
    gpu_gp0(g, 0x65000000); gpu_gp0(g, xy(12, 96));
    gpu_gp0(g, (300 * 64 + 32) << 16); gpu_gp0(g, xy(64, 48));
    gpu_gp0(g, 0xe2000000);
    gpu_gp0(g, 0x2d000000); gpu_gp0(g, xy(108, 96)); gpu_gp0(g, (300 * 64 + 32) << 16);
    gpu_gp0(g, xy(190, 104)); gpu_gp0(g, (0x1a << 16) | 32);
    gpu_gp0(g, xy(100, 144)); gpu_gp0(g, 32 << 8);
    gpu_gp0(g, xy(190, 148)); gpu_gp0(g, (32 << 8) | 32);
    /* Gouraud polyline with inclusive endpoints. */
    gpu_gp0(g, 0x580000ff); gpu_gp0(g, xy(214, 100));
    gpu_gp0(g, 0x0000ff00); gpu_gp0(g, xy(304, 140));
    gpu_gp0(g, 0x00ff0000); gpu_gp0(g, xy(214, 140));
    gpu_gp0(g, 0x00ffffff); gpu_gp0(g, xy(304, 100)); gpu_gp0(g, 0x50005000);
    /* Four panels: half blend, add, subtract, quarter add. */
    for (i = 0; i < 4; ++i) {
        gpu_gp0(g, 0xe1000400 | (i << 5));
        rectangle(g, 12 + (int)i * 76, 168, 64, 56, 0x906030, 0);
        rectangle(g, 20 + (int)i * 76, 180, 48, 32, 0x4060c0, 1);
    }
    gpu_gp1(g, 0x05000000);
    gpu_gp1(g, 0x06000000 | 600 | (3160 << 12));
    gpu_gp1(g, 0x07000000 | 16 | (256 << 10));
    gpu_gp1(g, 0x08000001); gpu_gp1(g, 0x03000000);
}

int main(int argc, char **argv) {
    const char *path = argc > 1 ? argv[1] : "build/gpu-demo.png";
    int repeats = argc > 2 ? atoi(argv[2]) : 1, i, ok;
    clock_t start;
    gpu_t *g = gpu_create();
    if (!g || repeats < 1 || repeats > 10000) { free(g); return 2; }
    start = clock();
    for (i = 0; i < repeats; ++i) scene(g);
    printf("GPU demo: %d scenes in %.3f CPU seconds\n", repeats, (double)(clock() - start) / CLOCKS_PER_SEC);
    ok = host_display_dump(g, path);
    free(g);
    if (!ok) { fprintf(stderr, "Cannot write %s\n", path); return 2; }
    printf("Demo PNG: %s\n", path);
    return 0;
}
