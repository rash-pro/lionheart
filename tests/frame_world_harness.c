/* A triangular patch loses its middle triangle when that triangle becomes
 * edge-on. The outer boundary stays closed at every presentation fraction. */
#include "frame_blend.h"
#include "gpu.h"
#include <string.h>
static int disable_net;
static float fractions[4][2], depths[4];
static unsigned precision_errors, precision_packets;
static void observe_fraction(gpu_t *g, float f[4][2]) {
    memcpy(fractions, f, sizeof(fractions)); gpu_subpixel(g, f);
}
static void observe_depth(gpu_t *g, const float d[4]) {
    memcpy(depths, d, sizeof(depths)); gpu_subpixel_depth(g, d);
}
#define gpu_subpixel observe_fraction
#define gpu_subpixel_depth observe_depth

static unsigned fixture_ground(const frame_blend_t *b, unsigned i, uint32_t num, uint32_t den, uint32_t words[14]) {
    return disable_net ? 0 : frame_blend_ground(b, i, num, den, words);
}
#define frame_blend_ground fixture_ground
#include "../runtime/frame_present.c"
#undef frame_blend_ground
#undef gpu_subpixel
#undef gpu_subpixel_depth
#include "gpu.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static void triangle(gpu_t *g, uint32_t colour, const uint32_t p[4]) {
    gpu_gp0(g, 0x20000000u | colour);
    for (unsigned c = 0; c < 3; ++c) gpu_gp0(g, p[c]);
}

static void clear(gpu_t *g) {
    gpu_gp0(g, 0xe3000000u);
    gpu_gp0(g, 0xe4000000u | 319u | 239u << 10);
    gpu_gp0(g, 0xe5000000u);
    gpu_gp0(g, 0x02000000u);
    gpu_gp0(g, 0);
    gpu_gp0(g, 320u | 240u << 16);
}

static void check_complete(void *context, const uint32_t *words, unsigned count) {
    complete(context, words, count);
    if (words[0] >> 24 < 0x20 || words[0] >> 24 >= 0x40) return;
    for (unsigned i = 0; i < 4; ++i)
        precision_errors += fractions[i][0] != 0 || fractions[i][1] != 0 || depths[i] != 0;
    ++precision_packets;
    memset(fractions, 0, sizeof(fractions)); memset(depths, 0, sizeof(depths));
}
static const gpu_observer_t precision_counter = {ignore_start, ignore_vblank, count_polygon, check_complete};

int main(int argc, char **argv) {

    static const int16_t points[4][3] = {{-160, 0, 1024}, {0, -160, 1024}, {160, 0, 1024}, {0, 0, 2048}};
    static const unsigned corners[3][3] = {{0, 1, 3}, {1, 2, 3}, {0, 3, 2}};
    frame_blend_t *b = frame_blend_create();
    gpu_t *g = gpu_create(), *reference = gpu_create();
    frame_identity_setup_t setup = {{4096, 0, 4096, 0, 4096}, {0, 0, 0}, 160 << 16, 120 << 16, 256, 12, 0};
    frame_identity_run_t run = {0, 4, 0, -1, -1, 0, 0};
    frame_identity_projection_t projections[4];
    frame_identity_polygon_t polygons[3];
    frame_identity_frame_t f;
    unsigned holes = 0, excess = 0, endpoints = 0, hidden = 0, visibility_errors = 0;
    int reversed = argc > 1 && !strcmp(argv[1], "reversed");
    int arriving = argc > 1 && !strcmp(argv[1], "arriving");
    int turn = argc > 1 && !strcmp(argv[1], "turn");
    int incoming = argc > 1 && (!strcmp(argv[1], "incoming") || !strcmp(argv[1], "precision-hidden"));
    int texture = argc > 1 && !strcmp(argv[1], "texture");
    int palette = argc > 1 && !strcmp(argv[1], "palette");
    int window = argc > 1 && !strcmp(argv[1], "window");
    int target = argc > 1 && !strcmp(argv[1], "target");
    int net = argc > 1 && !strncmp(argv[1], "net", 3);
    if (!b || !g || !reference) return 2;
    memset(&f, 0, sizeof(f));
    f.tag = FRAME_IDENTITY_POINTS;
    f.setup_count = f.run_count = 1; f.projection_count = 4;
    f.setups = &setup; f.runs = &run; f.projections = projections; f.polygons = polygons;
    if (net && !strcmp(argv[1], "net-model")) setup.translation[0] = 32;
    for (unsigned frame = 0; frame < 2; ++frame) {
        int offset = frame ? (reversed ? -32 : 0) : 32;
        if (incoming) offset = frame ? 32 : -32;
        memset(projections, 0, sizeof(projections));
        memset(polygons, 0, sizeof(polygons));
        if (turn && frame) {
            setup.rotation[0] = setup.rotation[4] = 4095;
            setup.rotation[1] = 64; setup.rotation[3] = (uint16_t)-64;
            setup.h = 248;
        }
        for (unsigned i = 0; i < 4; ++i) {
            frame_identity_projection_t *p = &projections[i];
            for (unsigned a = 0; a < 3; ++a) p->input[a] = points[i][a] + (a == 1 ? offset : 0);
            for (unsigned a = 0; a < 3; ++a) {
                int64_t v = 0;
                for (unsigned j = 0; j < 3; ++j) {
                    unsigned index = a * 3 + j;
                    v += (int64_t)(int16_t)(setup.rotation[index / 2] >> ((index % 2) * 16)) * p->input[j];
                }
                p->view[a] = (int32_t)(v >= 0 ? v / 4096 : -1 - (-1 - v) / 4096);
            }
            p->previous = p->match = frame ? (int32_t)i : -1;
            p->sxy = frame_blend_project(p->view, setup.ofx, setup.ofy, setup.h, setup.shift, setup.lm);
            p->sz = (uint16_t)p->view[2];
        }
        f.number = frame; f.vblank = frame * 2; f.first_polygon = frame * 3; f.flags = frame ? 0 : FRAME_IDENTITY_RESET;
        f.polygon_count = incoming ? (frame ? 3 : 2) : (frame ? 2 : 3);
        if (net) f.polygon_count = 2; /* No outgoing pass can mask a consumed serial. */
        if (frame && (texture || palette || target)) {
            uint32_t upload[4] = {0xa0000000u, 512u | (palette ? 255u << 16 : 0), 1u | 1u << 16, 0};
            if (target) {
                uint32_t command = 0xe3000000u | 512u;
                frame_blend_command(b, &command, 1);
                command = 0xe4000000u | 527u | 15u << 10;
                frame_blend_command(b, &command, 1);
                upload[0] = 0x20008080u;
                upload[1] = 512u; upload[2] = 527u; upload[3] = 512u | 15u << 16;
            }
            frame_blend_command(b, upload, target ? 4 : 3);
        }
        if (frame && window) {
            uint32_t command = 0xe2000001u;
            frame_blend_command(b, &command, 1);
        }
        for (unsigned i = 0; i < f.polygon_count; ++i) {
            uint32_t packet[7] = {0x20008080u, 0, 0, 0, 0, 0, 0};
            int textured = texture || palette || window || target;
            if (textured) packet[0] |= 0x04000000u;
            if (net && !strcmp(argv[1], "net-translucent")) packet[0] |= 0x02000000u;
            polygons[i].opcode = packet[0] >> 24; polygons[i].corners = 3;
            for (unsigned c = 0; c < 3; ++c) {
                polygons[i].projection[c] = (int32_t)corners[i][c];
                packet[1 + c * (textured ? 2 : 1)] = polygons[i].xy[c] = projections[corners[i][c]].sxy;
            }
            if (textured) {
                packet[2] = (32u | 255u << 6) << 16;
                packet[4] = 8u << 16 | 8u;
                packet[6] = 8u << 8;
            }
            frame_blend_packet(b, f.first_polygon + i, packet);
        }
        if (arriving && !frame) {
            polygons[2].state[0] = FRAME_IDENTITY_UNPAIRED;
            polygons[2].projection[0] = -1;
        }
        if (!frame_blend_add(b, &f)) return 2;
    }
    if (argc > 1 && !strcmp(argv[1], "precision-hidden")) {
        frame_present_t presenter = {0};
        at_t at = {0};
        presenter.gpu = g; presenter.blend = b; presenter.precision = 2; presenter.fractions = 1;
        gpu_observe(g, &precision_counter, &presenter);
        for (unsigned num = 1; num < 256; ++num) {
            if (frame_blend_visible(b, f.first_polygon + 2, num, 256)) continue;
            uint8_t packet[16];
            float corners[4][3] = {{160.25f, 120.125f, 1000}, {170.5f, 130.25f, 1100}, {150.75f, 140.5f, 1200}};
            presenter.serial = f.first_polygon + 2;
            frame_present_corners(&presenter, presenter.serial, 7, corners);
            put_le32(packet, 0x20008080u);
            for (unsigned c = 0; c < 3; ++c) put_le32(packet + 4 + c * 4, polygons[2].xy[c]);
            at.num = num; at.den = 256; at.beneath = 1;
            feed(&presenter, packet, 4, &at);
        }
        unsigned pixels = 0;
        for (unsigned i = 0; i < 1024 * 512; ++i) pixels += gpu_vram(g)[i] != 0;
        printf("precision-packets=%u errors=%u pixels=%u\n", precision_packets, precision_errors, pixels);
        free(presenter.corners); free(g); free(reference); frame_blend_destroy(b);
        return !precision_packets || precision_errors || pixels;
    }
    if (net) {
        unsigned errors = 0, covered = 0;
        int excluded = !strcmp(argv[1], "net-model") || !strcmp(argv[1], "net-translucent");
        uint32_t words[14], before[4], after[4];
        memcpy(before, polygons[1].xy, sizeof(before));
        frame_blend_polygon(b, f.first_polygon + 1, 1, 2, before, NULL);
        clear(g);
        frame_present_t presenter = {0};
        at_t at = {0};
        presenter.gpu = g; presenter.blend = b; presenter.serial = f.first_polygon + 1;
        at.num = 1; at.den = 2;
        int precise_net = !strcmp(argv[1], "net-precision");
        if (precise_net) {
            float corners[4][3] = {{160.25f, 120.125f, 1000}, {170.5f, 130.25f, 1100}, {150.75f, 140.5f, 1200}};
            presenter.precision = 2; presenter.fractions = 1;
            frame_present_corners(&presenter, presenter.serial, 7, corners);
        }
        gpu_observe(g, precise_net ? &precision_counter : &counter, &presenter);

        disable_net = !strcmp(argv[1], "net-disabled");
        beneath(&presenter, &at);
        errors += presenter.serial != f.first_polygon + 1;
        if (precise_net) {
            errors += precision_errors || !precision_packets;
            float after_fraction[4][2], after_depth[4];
            uint8_t packet[16];
            put_le32(packet, 0x20008080u);
            for (unsigned c = 0; c < 3; ++c) put_le32(packet + 4 + c * 4, polygons[1].xy[c]);
            gpu_observe(g, &counter, &presenter);
            feed(&presenter, packet, 4, &at);
            memcpy(after_fraction, fractions, sizeof(fractions)); memcpy(after_depth, depths, sizeof(depths));
            presenter.serial = f.first_polygon + 1;
            memset(fractions, 0, sizeof(fractions)); memset(depths, 0, sizeof(depths));
            feed(&presenter, packet, 4, &at);
            errors += memcmp(after_fraction, fractions, sizeof(fractions)) != 0;
            errors += memcmp(after_depth, depths, sizeof(depths)) != 0;
            errors += depths[0] <= 0 || fractions[0][0] == 0;
            presenter.serial = f.first_polygon + 1;
            free(presenter.corners);
        }

        for (unsigned i = 0; i < f.polygon_count; ++i) {
            unsigned n = frame_blend_ground(b, i, 1, 2, words);
            errors += excluded ? n != 0 : n == 0;
            errors += frame_blend_ground(b, i, 0, 2, words) != 0;
            errors += frame_blend_ground(b, i, 2, 2, words) != 0;
        }
        memcpy(after, polygons[1].xy, sizeof(after));
        frame_blend_polygon(b, presenter.serial, 1, 2, after, NULL);
        errors += memcmp(before, after, sizeof(before)) != 0;
        for (unsigned y = 0; y < 240; ++y)
            for (unsigned x = 0; x < 320; ++x) covered += gpu_vram(g)[y * 1024 + x] != 0;
        if (!strcmp(argv[1], "net-model") || !strcmp(argv[1], "net-translucent")) {
            clear(reference);
            triangle(g, 0x8080u, after);
            triangle(reference, 0x8080u, before);
            errors += memcmp(gpu_vram(g), gpu_vram(reference), 1024 * 512 * sizeof(uint16_t)) != 0;
        }
        if (!strcmp(argv[1], "net") || disable_net) {
            clear(reference);
            /* This omitted face is degenerate at both game frames:
             * (160,100),(170,110),(150,90) and
             * (160,100),(150,110),(150,110). Its midpoint has area 100,
             * despite neither endpoint submitting a packet for it. */
            const uint32_t missing[4] = {160u | 100u << 16, 160u | 110u << 16, 150u | 100u << 16, 0};
            triangle(reference, 0x8080u, missing);
            for (unsigned y = 0; y < 240; ++y)
                for (unsigned x = 0; x < 320; ++x)
                    errors += gpu_vram(reference)[y * 1024 + x] &&
                              gpu_vram(g)[y * 1024 + x] != gpu_vram(reference)[y * 1024 + x];
        }
        printf("net-covered=%u errors=%u\n", covered, errors);
        free(g); free(reference); frame_blend_destroy(b);
        return errors || (excluded ? covered != 0 : !covered);
    }
    if (texture || palette || window || target) {
        unsigned count = frame_blend_leftovers(b);
        printf("unsafe-continuations=%u\n", count);
        free(g); free(reference); frame_blend_destroy(b);
        return count != 0;
    }
    for (unsigned num = 0; num <= 256; ++num) {
        uint32_t outer[4], positions[2][4];
        clear(g); clear(reference);
        for (unsigned i = 0; i < frame_blend_leftovers(b); ++i) {
            uint32_t packet[12];
            unsigned n = frame_blend_leftover(b, i, num, 256, packet);
            if (!n && num < 256) ++hidden;
            for (unsigned k = 0; k < n; ++k) gpu_gp0(g, packet[k]);
            if (n && num == 256) ++endpoints;
        }
        for (unsigned i = 0; i < 2; ++i) {
            memcpy(positions[i], polygons[i].xy, sizeof(positions[i]));
            frame_blend_polygon(b, f.first_polygon + i, num, 256, positions[i], NULL);
            triangle(g, 0x8080u, positions[i]);
            triangle(reference, 0x8080u, positions[i]);
        }
        if (incoming) {
            uint32_t xy[4];
            memcpy(xy, polygons[2].xy, sizeof(xy));
            frame_blend_polygon(b, f.first_polygon + 2, num, 256, xy, NULL);
            int x[3], y[3];
            for (unsigned c = 0; c < 3; ++c) {
                x[c] = (int16_t)xy[c]; y[c] = (int16_t)(xy[c] >> 16);
            }
            int front = (x[1] - x[0]) * (y[2] - y[0]) > (y[1] - y[0]) * (x[2] - x[0]);
            int visible = frame_blend_visible(b, f.first_polygon + 2, num, 256);
            visibility_errors += visible != front;
            if (visible) triangle(g, 0x8080u, xy);
            else ++hidden;
        }
        outer[0] = positions[0][0]; outer[1] = positions[0][1]; outer[2] = positions[1][1];
        triangle(reference, 0x8080u, outer);
        const uint16_t *actual = gpu_vram(g), *expected = gpu_vram(reference);
        for (unsigned y = 0; y < 240; ++y)
            for (unsigned x = 0; x < 320; ++x)
            {
                holes += expected[y * 1024 + x] && !actual[y * 1024 + x];
                excess += actual[y * 1024 + x] && !expected[y * 1024 + x];
            }
    }
    printf("holes=%u endpoint-extras=%u hidden=%u excess=%u visibility-errors=%u\n",
           holes, endpoints, hidden, excess, visibility_errors);
    free(g); free(reference); frame_blend_destroy(b);
    return holes || excess || endpoints || visibility_errors || ((reversed || incoming) && !hidden);
}
