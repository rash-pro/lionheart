/* Drives frame identity, frame blending, frame layers and the recorder with a
 * synthetic field, no game data: a background of 16x16 tiles from a texture
 * the harness uploads, scrolled by the drawing offset as a field scrolls, a
 * quad projected by the GTE that walks across it with the scroll following,
 * and a tile drawn over the quad (a mask). For each game frame it prints what
 * the layers give, and checks at alpha 1/4, 1/2 and 3/4 where things stand on
 * screen (drawing offset plus packet position):
 *   L<number> reason=<why> move=<dx>,<dy> tiles=<n> paired=<n> unpaired=<n> disagree=<n> untextured=<n> layers=<n>
 *   A<number> layer=<ok|differ> mask=<ok|differ> relative=<ok|differ> special=<ok|differ>
 * layer: every background tile at the fraction of its move; mask: the mask
 * with them; relative: each quad corner against the tile under it, at the
 * fraction of their relative move (no sliding); special: the scenario's own
 * tile where the scenario says it stands (the animated one moves with the
 * layer, the one that moves by itself stays where the newer frame has it,
 * the parallax ones at the fraction of their own move). A frame that does
 * not move prints A<number> held.
 * Scenarios: scroll, animated, disagree, parallax, change, rigid, edge, reset.
 * Usage: frame_layer_harness SCENARIO [RECORDING] */
#include "frame_blend.h"
#include "frame_identity.h"
#include "frame_layer.h"
#include "frame_record.h"
#include "gpu.h"
#include "gte.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static cpu_t *c;
static gpu_t *g;
static frame_blend_t *blend;
static frame_layer_t *layer;
static unsigned back = 1, tag = 1;
static int failures;
static const char *scenario;

static const char *const reasons[FRAME_LAYER_REASONS] = {"moved", "no-history", "still", "changed", "not-rigid", "edge", "few"};

enum { TILE, MASK, SPECIAL, HUD };

/* What one game frame sent: its sprites in order, its field offset and its quad. */
typedef struct {
    int x, y, kind, id;
} sent_sprite_t;

typedef struct {
    sent_sprite_t sprites[1024];
    unsigned sprite_count, offsets, field_offset;
    int rel_x, rel_y;           /* the field offset from the drawing area's corner */
    uint32_t quad[4];
    uint64_t quad_serial;
} sent_t;
static sent_t frames[2], *cur = &frames[0], *before = &frames[1];
static uint64_t polygons;

static void command(const uint32_t *w, unsigned n) {
    unsigned i;
    for (i = 0; i < n; ++i) gpu_gp0(g, w[i]);
    if (!frame_layer_command(layer, w, n)) { printf("FAIL out of memory\n"); ++failures; }
    if (w[0] >> 24 == 0xe5) ++cur->offsets;
}

static void one(uint32_t w) { command(&w, 1); }

static void sprite_words(const uint32_t *w, unsigned n, int x, int y, int kind, int id) {
    if (cur->sprite_count < 1024) {
        cur->sprites[cur->sprite_count].x = x; cur->sprites[cur->sprite_count].y = y;
        cur->sprites[cur->sprite_count].kind = kind; cur->sprites[cur->sprite_count].id = id;
        ++cur->sprite_count;
    }
    command(w, n);
}

static void sprite(int x, int y, unsigned u, unsigned v, int kind, int id) {
    uint32_t w[3];
    w[0] = 0x7c808080u;                                     /* 16x16, textured */
    w[1] = ((uint32_t)x & 0xffffu) | ((uint32_t)y & 0xffffu) << 16;
    w[2] = (u & 255u) | (v & 255u) << 8;
    /* The texture holds fewer tiles than the screen: where the scenario must
     * tell every tile apart, a palette field the 15-bit texture ignores does. */
    if (!strcmp(scenario, "rigid") && kind == TILE) w[2] |= (uint32_t)(id + 1) << 16;
    sprite_words(w, 3, x, y, kind, id);
}

static int32_t between(int32_t a, int32_t b, uint32_t num, uint32_t den) {
    int64_t d = ((int64_t)b - a) * num;
    return (int32_t)(a + (d >= 0 ? (d + den / 2) / den : -((-d + den / 2) / (int64_t)den)));
}

static int16_t low(uint32_t w) { return (int16_t)(uint16_t)(w & 0xffffu); }
static int16_t high(uint32_t w) { return (int16_t)(uint16_t)(w >> 16); }
static int32_t extend11(uint32_t v) { return (int32_t)(v << 21) >> 21; }

/* Where the latest frame's field offset stands at alpha, from its corner. */
static void offset_at(uint32_t num, uint32_t den, int *x, int *y) {
    uint32_t word;
    *x = cur->rel_x; *y = cur->rel_y;
    if (frame_layer_offset(layer, cur->field_offset, num, den, &word)) {
        *x = extend11(word) - 0; *y = extend11(word >> 11) - (int)(back * 256u);
    }
}

static const sent_sprite_t *find_before(int id) {
    unsigned i;
    for (i = 0; i < before->sprite_count; ++i)
        if (before->sprites[i].id == id && before->sprites[i].kind != HUD) return &before->sprites[i];
    return NULL;
}

/* The scenario's own tile moves by this much a frame in the picture. */
static int special_step(void) {
    if (!strcmp(scenario, "disagree")) return 5;
    if (!strcmp(scenario, "parallax")) return 3;
    return 0;
}

static void check(uint64_t number) {
    const frame_layer_info_t *li = frame_layer_info(layer);
    static const uint32_t quarters[3] = {1, 2, 3};
    int layer_ok = 1, mask_ok = 1, relative_ok = 1, special_ok = 1, compared;
    unsigned q, i, k;
    printf("L%llu reason=%s move=%d,%d tiles=%u paired=%u unpaired=%u disagree=%u untextured=%u layers=%u\n",
           (unsigned long long)number, reasons[li->reason], li->dx, li->dy, li->tiles, li->paired, li->unpaired,
           li->disagree, li->untextured, li->layers);
    if (li->reason != FRAME_LAYER_MOVED) {
        /* Held: every offset and sprite as the newer frame has it. */
        uint32_t word;
        int same = 1;
        for (q = 0; q < 3; ++q) {
            for (i = 0; i < cur->offsets; ++i) if (frame_layer_offset(layer, i, quarters[q], 4, &word)) same = 0;
            for (i = 0; i < cur->sprite_count; ++i) {
                uint32_t xy = ((uint32_t)cur->sprites[i].x & 0xffffu) | ((uint32_t)cur->sprites[i].y & 0xffffu) << 16;
                word = xy;
                if (frame_layer_sprite(layer, i, quarters[q], 4, &word) || word != xy) same = 0;
            }
        }
        printf("A%llu held%s\n", (unsigned long long)number, same ? "" : " differ");
        return;
    }
    for (q = 0; q < 3; ++q) {
        int ox, oy;
        uint32_t quad[4];
        unsigned mask;
        offset_at(quarters[q], 4, &ox, &oy);
        for (k = 0; k < 4; ++k) quad[k] = cur->quad[k];
        mask = frame_blend_polygon(blend, cur->quad_serial, quarters[q], 4, quad, NULL);
        compared = 0;
        for (i = 0; i < cur->sprite_count; ++i) {
            const sent_sprite_t *b = find_before(cur->sprites[i].id);
            uint32_t xy = ((uint32_t)cur->sprites[i].x & 0xffffu) | ((uint32_t)cur->sprites[i].y & 0xffffu) << 16;
            int sx, sy, nx = cur->sprites[i].x + cur->rel_x, ny = cur->sprites[i].y + cur->rel_y, fx, fy, ex, ey;
            int kind = cur->sprites[i].kind;
            if (kind == HUD) continue;
            frame_layer_sprite(layer, i, quarters[q], 4, &xy);
            sx = low(xy) + ox; sy = high(xy) + oy;
            /* Where it stood in the frame before; a tile with no pair stands
             * where the layer's move puts it. */
            if (b) { fx = b->x + before->rel_x; fy = b->y + before->rel_y; }
            else { fx = nx - li->dx; fy = ny - li->dy; }
            ex = between(fx, nx, quarters[q], 4); ey = between(fy, ny, quarters[q], 4);
            if (kind == SPECIAL && !strcmp(scenario, "disagree")) { ex = nx; ey = ny; }
            if (abs(sx - ex) > 1 || abs(sy - ey) > 1) {
                if (kind == TILE) layer_ok = 0;
                else if (kind == MASK) mask_ok = 0;
                else special_ok = 0;
            }
            /* The quad's corners against the first background tile both
             * frames draw. */
            if (kind == TILE && b && !compared) {
                compared = 1;
                for (k = 0; k < 4; ++k) {
                    int rx = low(quad[k]) + ox - sx, ry = high(quad[k]) + oy - sy;
                    int bx = low(before->quad[k]) - b->x, by = high(before->quad[k]) - b->y;
                    int nxr = low(cur->quad[k]) - cur->sprites[i].x, nyr = high(cur->quad[k]) - cur->sprites[i].y;
                    if (abs(rx - between(bx, nxr, quarters[q], 4)) > 1 || abs(ry - between(by, nyr, quarters[q], 4)) > 1)
                        relative_ok = 0;
                }
            }
        }
        if (mask != 15 || !compared) relative_ok = 0;
    }
    printf("A%llu layer=%s mask=%s relative=%s special=%s\n", (unsigned long long)number, layer_ok ? "ok" : "differ",
           mask_ok ? "ok" : "differ", relative_ok ? "ok" : "differ", special_ok ? "ok" : "differ");
}

static void sink(void *context, const frame_identity_frame_t *f) {
    sent_t *t;
    (void)context;
    frame_record_frame(f);
    if (!frame_blend_add(blend, f)) { printf("FAIL out of memory\n"); ++failures; return; }
    /* Field-to-field changes, resets and cuts are resets for the layers too. */
    if (!frame_layer_end(layer, frame_blend_info(blend)->reason == FRAME_BLEND_KEPT)) { printf("FAIL layer\n"); ++failures; }
    if (cur->sprite_count) check(f->number);
    t = before; before = cur; cur = t;
    memset(cur, 0, sizeof(*cur));
}

static uint32_t classify(void *context, const cpu_t *cpu) { (void)context; (void)cpu; return tag; }

static void camera(int tx, int tz) {
    unsigned i;
    for (i = 0; i < 5; ++i) c->gte_ctrl[i] = 0;
    c->gte_ctrl[0] = 4096; c->gte_ctrl[2] = 4096; c->gte_ctrl[4] = 4096;
    c->gte_ctrl[5] = (uint32_t)tx; c->gte_ctrl[6] = 0; c->gte_ctrl[7] = (uint32_t)tz;
    c->gte_ctrl[24] = 160u << 16; c->gte_ctrl[25] = 120u << 16; c->gte_ctrl[26] = 300;
}

static uint32_t rtps(int x, int y, int z) {
    c->gte_data[0] = (uint32_t)(uint16_t)x | (uint32_t)(uint16_t)y << 16;
    c->gte_data[1] = (uint32_t)(uint16_t)z;
    frame_identity_gte_command(c, (1u << 19) | 0x01);
    return c->gte_data[14];
}

static void display(void) {
    gpu_gp1(g, 0x08000001u);
    gpu_gp1(g, 0x06000000u | 0x260u | (0x260u + 2560u) << 12);
    gpu_gp1(g, 0x07000000u | 16u | (16u + 240u) << 10);
    gpu_gp1(g, 0x05000000u);
    gpu_gp1(g, 0x03000000u);
}

/* A 256x256 15-bit texture at (512, 0): every 16x16 tile of it differs. */
static void texture(void) {
    unsigned x, y;
    gpu_gp0(g, 0xa0000000u); gpu_gp0(g, 512u); gpu_gp0(g, 256u << 16 | 256u);
    for (y = 0; y < 256; ++y)
        for (x = 0; x < 256; x += 2) {
            uint32_t a = (x * 7u + y * 13u + (x >> 4) * 31u + (y >> 4) * 17u) & 0x7fffu, b = ((x + 1) * 7u + y * 13u + (x >> 4) * 31u + (y >> 4) * 17u) & 0x7fffu;
            gpu_gp0(g, (a ? a : 1u) | (b ? b : 1u) << 16);
        }
}

/* One game frame of the field: the camera scrolled by scroll pixels, the quad
 * at walk in the picture, the scenario's tiles. */
static void field_frame(int f, int scroll, int walk, unsigned vblanks) {
    int i, j, cols, rows;
    uint32_t quad_xy[4];
    static const int corners[4][2] = {{0, 0}, {48, 0}, {0, 72}, {48, 72}};
    int changed = !strcmp(scenario, "change") && f >= 4;
    int edge = !strcmp(scenario, "edge");
    unsigned page = changed ? 1u : 0u;
    one(0xe3000000u | (back * 256u) << 10);
    one(0xe4000000u | 319u | (back * 256u + 239u) << 10);
    one(0xe1000000u | 8u | 2u << 7);
    cur->field_offset = cur->offsets;
    cur->rel_x = -scroll; cur->rel_y = 0;
    one(0xe5000000u | ((uint32_t)-scroll & 0x7ffu) | (back * 256u) << 11);
    /* The background, as a field draws it: only the tiles its scroll shows,
     * with one to spare at each side (none in the edge scenario). */
    cols = 32; rows = 16;
    for (j = 0; j < rows; ++j)
        for (i = 0; i < cols; ++i) {
            int x = i * 16 - 16, y = j * 16 - 16, sx = x - scroll, id = j * cols + i;
            /* The texture's last row is the scenario's own and the mask's. */
            unsigned u = (unsigned)(i * 16 + (int)page * 8), v = (unsigned)(j % 15 * 16);
            if (edge ? (sx <= -16 || sx >= 320 || y <= -16) : (sx < -32 || sx >= 336)) continue;
            if (!strcmp(scenario, "animated") && id == 2 * cols + 12) u = (unsigned)(f % 2 ? 0 : 128) + 64;
            if (!strcmp(scenario, "rigid") && id % 2 == 0) {
                unsigned h = (unsigned)(f * 2654435761u) ^ (unsigned)(id * 40503u);
                x += (int)((h >> 7) % 13u); y += (int)((h >> 17) % 7u);
            }
            sprite(x, y, u, v, TILE, id);
        }
    /* The scenario's own tiles: one that moves by itself, or a layer of ten
     * that moves at its own rate. */
    if (special_step()) {
        unsigned n = !strcmp(scenario, "parallax") ? 10 : 1;
        for (i = 0; i < (int)n; ++i) sprite(40 + i * 16 + f * special_step(), 180, (unsigned)(i * 16), 240, SPECIAL, 10000 + i);
    }
    /* The quad walks right; the field scrolls to follow it. */
    camera(walk * 3, 900);
    for (i = 0; i < 4; ++i) quad_xy[i] = rtps(corners[i][0] - 150, corners[i][1] - 60, 0);
    cur->quad_serial = polygons;
    for (i = 0; i < 4; ++i) cur->quad[i] = quad_xy[i];
    {
        uint32_t w[5] = {0x28c00000u, quad_xy[0], quad_xy[1], quad_xy[2], quad_xy[3]};
        command(w, 5);
        ++polygons;
    }
    /* A mask: a tile of the scenery drawn over the quad. */
    sprite(100, 40, 224, 240, MASK, 20000);
    gpu_gp1(g, 0x05000000u | (back * 256u) << 10);
    /* What the shown VBlank adds: a HUD in the picture shown, no offset. */
    one(0xe3000000u | (back * 256u) << 10);
    one(0xe4000000u | 319u | (back * 256u + 239u) << 10);
    one(0xe5000000u | (back * 256u) << 11);
    {
        uint32_t w[3] = {0x60ffffffu, (8u << 16) | 8u, (8u << 16) | 40u};
        sprite_words(w, 3, 8, 8, HUD, 30000);
    }
    back ^= 1;
    for (i = 0; i < (int)vblanks; ++i) gpu_vblank(g);
}

int main(int argc, char **argv) {
    const char *record = argc > 2 ? argv[2] : NULL;
    frame_identity_config_t config;
    int f;
    scenario = argc > 1 ? argv[1] : "";
    if (strcmp(scenario, "scroll") && strcmp(scenario, "animated") && strcmp(scenario, "disagree") &&
        strcmp(scenario, "parallax") && strcmp(scenario, "change") && strcmp(scenario, "rigid") &&
        strcmp(scenario, "edge") && strcmp(scenario, "reset")) {
        fprintf(stderr, "unknown scenario %s\n", scenario); return 2;
    }
    c = calloc(1, sizeof(*c));
    g = gpu_create();
    blend = frame_blend_create();
    layer = frame_layer_create();
    if (!c || !g || !blend || !layer) return 2;
    display();
    texture();
    memset(&config, 0, sizeof(config));
    config.sink = sink; config.classify = classify;
    frame_identity_start(g, &config);
    gpu_gp1(g, 0x05000000u);
    if (record && !frame_record_start(g, record, 1, 0)) { fprintf(stderr, "cannot record to %s\n", record); return 2; }
    for (f = 0; f < 8; ++f) {
        /* After the fourth frame the reset scenario leaves a gap, as a
         * field-to-field change does while the next field loads. */
        unsigned vblanks = !strcmp(scenario, "reset") && f == 4 ? 12 : 2;
        field_frame(f, f * 6, f * 6, vblanks);
    }
    gpu_gp1(g, 0x05000000u | (back * 256u) << 10);
    if (record && !frame_record_stop()) { printf("FAIL recording\n"); ++failures; }
    frame_identity_stop();
    frame_blend_destroy(blend);
    frame_layer_destroy(layer);
    free(c);
    return failures ? 1 : 0;
}
