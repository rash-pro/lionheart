/* Draws the frames between a game's frames from a recording (--frame-record,
 * docs/frame-blend.md) through the software rasterizer, and checks and
 * measures them:
 *   inbetween RECORD [--from V] [--until V] [--out DIR] [--canvas WxH] [--sheet V] [--frames]
 * Every VBlank of the recording is replayed and must show what the game showed.
 * Every game frame is drawn again with its polygons where they stand at the end
 * of its interval (alpha 1), which must give the game's picture pixel for
 * pixel, and at its start (alpha 0), which is compared with the picture before
 * it: what differs there still moves only at the game's rate. Between from and
 * until, --out writes off.rgb (the game's pictures, one per VBlank) and on.rgb
 * (the blended ones) as raw 24-bit frames of the canvas size, and heat.ppm:
 * where pixels changed at game frames (red) or between them (green). --sheet
 * writes the pictures of the first blended game frame from VBlank V on, and
 * --frames prints what blending gave each game frame.
 * The report goes to stdout; the exit status is 1 if a check failed. */
#include "../../runtime/gpu.c"
#include "frame_blend.h"
#include "frame_record.h"
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { WIDTH = 1024, HEIGHT = 512 };

typedef struct {
    uint8_t *data;
    size_t size, capacity;
} bytes_t;

static int append(bytes_t *b, const void *p, size_t n) {
    if (b->size + n > b->capacity) {
        size_t c = b->capacity ? b->capacity * 2 : 1 << 20;
        uint8_t *d;
        while (c < b->size + n) c *= 2;
        if (!(d = realloc(b->data, c))) return 0;
        b->data = d; b->capacity = c;
    }
    memcpy(b->data + b->size, p, n);
    b->size += n;
    return 1;
}

static uint32_t le32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static uint64_t le64(const uint8_t *p) { return le32(p) | (uint64_t)le32(p + 4) << 32; }
static void put_le32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); }

/* A GPU that counts the polygons it draws, as the identity service does. */
typedef struct {
    gpu_t *gpu;
    uint64_t serial;
} counted_t;
static void count_polygon(void *context, unsigned opcode, const uint32_t *packet) {
    (void)opcode; (void)packet;
    ++((counted_t *)context)->serial;
}
static void ignore_start(void *context, uint32_t start) { (void)context; (void)start; }
static void ignore_vblank(void *context) { (void)context; }
static const gpu_observer_t counter = {ignore_start, ignore_vblank, count_polygon};

/* A GPU state with the serial of its next polygon. */
typedef struct {
    bytes_t state;
    uint64_t serial;
    int valid;
} checkpoint_t;

static int save(checkpoint_t *k, counted_t *g) {
    ss_io io;
    memset(&io, 0, sizeof(io));
    ss_gpu(&io, g->gpu);
    if (io.failed) { free(io.data); return 0; }
    free(k->state.data);
    k->state.data = io.data; k->state.size = io.size; k->state.capacity = io.capacity;
    k->serial = g->serial; k->valid = 1;
    return 1;
}

static int restore(counted_t *g, const uint8_t *state, size_t size, uint64_t serial) {
    ss_io io;
    memset(&io, 0, sizeof(io));
    io.data = (uint8_t *)state; io.size = size; io.reading = 1; io.version = 1;
    ss_gpu(&io, g->gpu);
    g->serial = serial;
    return !io.failed && io.pos == size;
}

/* The display area as the host presents it, centred on a canvas. */
static void expand(uint16_t v, uint8_t *o) {
    unsigned i;
    for (i = 0; i < 3; ++i) { unsigned x = (v >> (i * 5)) & 31; o[i] = (uint8_t)((x << 3) | (x >> 2)); }
}
static void picture(gpu_t *g, uint8_t *out, int cw, int ch) {
    int x, y, w, h, rgb24, enabled, i, j;
    const uint16_t *vram = gpu_vram(g);
    memset(out, 0, (size_t)cw * ch * 3);
    gpu_display(g, &x, &y, &w, &h, &rgb24, &enabled);
    if (!enabled) return;
    for (j = 0; j < h; ++j) {
        int oy = j + (ch - h) / 2;
        if (oy < 0 || oy >= ch) continue;
        for (i = 0; i < w; ++i) {
            int ox = i + (cw - w) / 2;
            uint8_t *o;
            unsigned row = ((unsigned)(y + j) & 511) * 1024;
            if (ox < 0 || ox >= cw) continue;
            o = out + ((size_t)oy * cw + ox) * 3;
            if (rgb24) {
                unsigned k, at = (unsigned)x * 2 + (unsigned)i * 3;
                for (k = 0; k < 3; ++k) { unsigned byte = (at + k) & 2047; o[k] = (uint8_t)(vram[row + byte / 2] >> ((byte & 1) * 8)); }
            } else expand(vram[row + ((unsigned)(x + i) & 1023)], o);
        }
    }
}

/* Replay. */
typedef struct {
    const frame_blend_t *blend;
    uint32_t num, den;
    unsigned stray;
    uint64_t split;          /* polygon packets cut between chunks, drawn as recorded */
} blend_at_t;

/* GP0 words, with the polygons of the blended frame where blending puts them. */
static void feed_gp0(counted_t *c, const uint8_t *p, unsigned words, blend_at_t *at) {
    gpu_t *g = c->gpu;
    while (words) {
        unsigned n = 1, op, need;
        if (g->transfer == 1) {
            unsigned left = g->tw * g->th - g->cursor;
            n = (left + 1) / 2;
            if (n > words) n = words;
            if (!n) n = 1;
        } else if (!g->count && !g->polyline) {
            op = p[3];
            need = packet_size(op);
            if (need <= words) {
                n = need;
                if (at && op >= 0x20 && op < 0x40) {
                    uint8_t packet[12 * 4];
                    uint32_t xy[4];
                    unsigned corners = (op & 8) ? 4 : 3, i, k = 1, mask;
                    memcpy(packet, p, n * 4);
                    mask = frame_blend_polygon(at->blend, c->serial, at->num, at->den, xy, &at->stray);
                    for (i = 0; i < corners; ++i) {
                        if (i && (op & 16)) ++k;
                        if (mask & (1u << i)) put_le32(packet + k * 4, xy[i]);
                        ++k;
                        if (op & 4) ++k;
                    }
                    gpu_gp0_words(g, packet, n);
                    p += n * 4; words -= n;
                    continue;
                }
            } else if (at && op >= 0x20 && op < 0x40) ++at->split;
        }
        gpu_gp0_words(g, p, n);
        p += n * 4; words -= n;
    }
}

/* One chunk of the recording, to a GPU. */
static void apply(counted_t *c, uint32_t tag, const uint8_t *p, size_t size, blend_at_t *at) {
    if (tag == FRAME_RECORD_GP0) feed_gp0(c, p, (unsigned)(size / 4), at);
    else if (tag == FRAME_RECORD_GP1 && size == 4) gpu_gp1(c->gpu, le32(p));
    else if (tag == FRAME_RECORD_READ) (void)gpu_read(c->gpu);
    else if (tag == FRAME_RECORD_VBLK) gpu_vblank(c->gpu);
}

/* Chunks since a checkpoint: tag, size, payload, one after another. */
typedef struct {
    bytes_t chunks;
    size_t mark;             /* where the next checkpoint's chunks begin */
} journal_t;

static int journal(journal_t *j, uint32_t tag, const uint8_t *p, size_t size) {
    uint8_t head[8];
    put_le32(head, tag); put_le32(head + 4, (uint32_t)size);
    return append(&j->chunks, head, 8) && append(&j->chunks, p, size);
}

static void replay(counted_t *c, const journal_t *j, blend_at_t *at) {
    size_t i = 0;
    while (i + 8 <= j->chunks.size) {
        uint32_t tag = le32(j->chunks.data + i), size = le32(j->chunks.data + i + 4);
        apply(c, tag, j->chunks.data + i + 8, size, at);
        i += 8 + size;
    }
}

static int write_ppm(const char *dir, const char *name, const uint8_t *rgb, int w, int h) {
    char path[4096];
    FILE *f;
    int ok;
    snprintf(path, sizeof(path), "%s/%s", dir, name);
    if (!(f = fopen(path, "wb"))) return 0;
    ok = fprintf(f, "P6\n%d %d\n255\n", w, h) > 0 && fwrite(rgb, 1, (size_t)w * h * 3, f) == (size_t)w * h * 3;
    return fclose(f) == 0 && ok;
}

static const char *const reasons[FRAME_BLEND_REASONS] = {"kept", "no-history", "excluded", "scene", "cut"};

int main(int argc, char **argv) {
    const char *path = NULL, *out = NULL;
    uint64_t from = 0, until = UINT64_MAX, sheet = 0;
    int cw = 320, ch = 240, i, failed = 0, have_sheet = 0, verbose = 0;
    frame_record_reader_t in;
    frame_record_frame_t frame;
    frame_blend_t *blend = frame_blend_create();
    counted_t truth, scratch;
    checkpoint_t a, b;
    journal_t j;
    FILE *off_file = NULL, *on_file = NULL;
    uint8_t *now, *before, *drawn, *start, *last;
    uint32_t *jumps, *smooth;
    uint64_t vblank = 0, frame_vblank = 0, previous_end = 0, interval = 0, last_step = 0, last_interval = 0;
    int have_frame = 0, interval_ready = 0, have_before = 0;
    /* Totals over the window. */
    uint64_t pictures = 0, picture_wrong = 0, frames = 0, frames_checked = 0, frames_wrong = 0, wrong_pixels = 0;
    uint64_t frames_zero_same = 0, changed = 0, jumped = 0, game_frames = 0, renders = 0, stray = 0, stray_frames = 0;
    uint64_t holds = 0, skips = 0, split = 0, reset[FRAME_BLEND_REASONS] = {0}, jumps_blended = 0, jumps_held = 0;
    uint64_t polygons_3d = 0, polygons_moving = 0, corners_3d = 0, corners_moving = 0;
    uint64_t held_service = 0, held_projection = 0, held_neighbour = 0, held_reset = 0, held_jump = 0;
    char wrong_list[1024] = "", stray_list[1024] = "", reset_list[2048] = "", jump_list[1024] = "";

    for (i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--from") && i + 1 < argc) from = strtoull(argv[++i], NULL, 10);
        else if (!strcmp(argv[i], "--until") && i + 1 < argc) until = strtoull(argv[++i], NULL, 10);
        else if (!strcmp(argv[i], "--out") && i + 1 < argc) out = argv[++i];
        else if (!strcmp(argv[i], "--frames")) verbose = 1;
        else if (!strcmp(argv[i], "--sheet") && i + 1 < argc) sheet = strtoull(argv[++i], NULL, 10);
        else if (!strcmp(argv[i], "--canvas") && i + 1 < argc) {
            if (sscanf(argv[++i], "%dx%d", &cw, &ch) != 2 || cw < 16 || ch < 16 || cw > WIDTH || ch > HEIGHT) {
                fprintf(stderr, "Invalid canvas: %s\n", argv[i]); return 2;
            }
        } else if (argv[i][0] != '-' && !path) path = argv[i];
        else {
            fprintf(stderr, "Usage: inbetween RECORD [--from V] [--until V] [--out DIR] [--canvas WxH] [--sheet V] [--frames]\n");
            return 2;
        }
    }
    if (!path || !blend || !frame_record_open(&in, path)) { fprintf(stderr, "Cannot read recording: %s\n", path ? path : ""); return 2; }
    memset(&frame, 0, sizeof(frame)); memset(&a, 0, sizeof(a)); memset(&b, 0, sizeof(b)); memset(&j, 0, sizeof(j));
    truth.gpu = gpu_create(); scratch.gpu = gpu_create(); truth.serial = scratch.serial = 0;
    now = calloc((size_t)cw * ch, 3); before = calloc((size_t)cw * ch, 3); drawn = calloc((size_t)cw * ch, 3);
    start = calloc((size_t)cw * ch, 3); last = calloc((size_t)cw * ch, 3);
    jumps = calloc((size_t)cw * ch, sizeof(*jumps)); smooth = calloc((size_t)cw * ch, sizeof(*smooth));
    if (!truth.gpu || !scratch.gpu || !now || !before || !drawn || !start || !last || !jumps || !smooth) return 2;
    gpu_observe(truth.gpu, &counter, &truth);
    gpu_observe(scratch.gpu, &counter, &scratch);
    if (out) {
        char file[4096];
        snprintf(file, sizeof(file), "%s/off.rgb", out); off_file = fopen(file, "wb");
        snprintf(file, sizeof(file), "%s/on.rgb", out); on_file = fopen(file, "wb");
        if (!off_file || !on_file) { fprintf(stderr, "Cannot write to %s\n", out); return 2; }
    }

    while (frame_record_next(&in)) {
        const uint8_t *p = in.payload;
        size_t size = in.size;
        if (in.tag == FRAME_RECORD_SNAP) {
            if (size < 24 || !restore(&truth, p + 24, size - 24, le64(p + 8))) { fprintf(stderr, "Damaged GPU state\n"); return 2; }
            frame_blend_reset(blend);
            a.valid = b.valid = 0; j.chunks.size = j.mark = 0;
            have_frame = interval_ready = have_before = 0;
        } else if (in.tag == FRAME_RECORD_FRAM) {
            const frame_blend_info_t *info;
            if (!frame_record_parse(&frame, p, size)) { fprintf(stderr, "Damaged identity frame\n"); return 2; }
            if (!frame_blend_add(blend, &frame.frame)) { fprintf(stderr, "Out of memory\n"); return 2; }
            info = frame_blend_info(blend);
            /* The new game frame shows from here; it was drawn since the
             * display start changed before, the checkpoint that becomes a. */
            if (b.valid) {
                checkpoint_t t = a; a = b; b = t;
                memmove(j.chunks.data, j.chunks.data + j.mark, j.chunks.size - j.mark);
                j.chunks.size -= j.mark;
            }
            if (!save(&b, &truth)) { fprintf(stderr, "Cannot keep the GPU state\n"); return 2; }
            j.mark = j.chunks.size;
            /* A game frame that comes before the one it follows reached
             * alpha 1 cuts that one short: the picture skips ahead. */
            if (last_step && last_step < last_interval) ++skips;
            interval = have_frame ? frame.frame.vblank - previous_end : 0;
            previous_end = frame.frame.vblank;
            have_frame = 1;
            frame_vblank = 0;
            interval_ready = a.valid && info->reason == FRAME_BLEND_KEPT && interval > 0 && interval <= FRAME_IDENTITY_GAP;
            last_step = 0; last_interval = interval;
            if (frame.frame.vblank + 1 >= from && frame.frame.vblank < until) {
                ++frames;
                ++reset[info->reason];
                if (info->reason != FRAME_BLEND_KEPT && strlen(reset_list) < sizeof(reset_list) - 40)
                    snprintf(reset_list + strlen(reset_list), sizeof(reset_list) - strlen(reset_list), " %" PRIu64 ":%s",
                             frame.frame.vblank + 1, reasons[info->reason]);
                if (verbose)
                    printf("frame %" PRIu64 " vblank=%" PRIu64 " reason=%s move=%u change=%u screen=%u moving=%u/%u polygons=%u/%u\n",
                           info->number, frame.frame.vblank + 1, reasons[info->reason], info->move, info->change, info->screen_move,
                           info->corners_moving, info->corners_3d, info->polygons_moving, info->polygons_3d);
                if (info->screen_move >= 32) {
                    /* Frame identity's summary counts these as jumps. */
                    if (info->reason == FRAME_BLEND_KEPT) {
                        ++jumps_blended;
                        if (strlen(jump_list) < sizeof(jump_list) - 40)
                            snprintf(jump_list + strlen(jump_list), sizeof(jump_list) - strlen(jump_list), " %" PRIu64 ":%upx",
                                     frame.frame.vblank + 1, info->screen_move);
                    } else ++jumps_held;
                }
                polygons_3d += info->polygons_3d; polygons_moving += info->polygons_moving;
                corners_3d += info->corners_3d; corners_moving += info->corners_moving;
                held_service += info->held_service; held_projection += info->held_projection;
                held_neighbour += info->held_neighbour; held_reset += info->held_reset; held_jump += info->held_jump;
            }
            continue; /* not journalled: it changes no GPU */
        } else if (in.tag == FRAME_RECORD_VBLK) {
            uint64_t recorded = size >= 16 ? le64(p + 8) : 0;
            int in_window, first_of_frame;
            vblank = size >= 8 ? le64(p) : vblank + 1;
            in_window = vblank >= from && vblank <= until;
            /* The picture the VBlank shows is taken before it changes the field. */
            if (frame_record_picture(truth.gpu) != recorded) {
                ++picture_wrong;
                if (strlen(wrong_list) < sizeof(wrong_list) - 40)
                    snprintf(wrong_list + strlen(wrong_list), sizeof(wrong_list) - strlen(wrong_list), " replay@%" PRIu64, vblank);
            }
            ++pictures;
            memcpy(before, now, (size_t)cw * ch * 3);
            picture(truth.gpu, now, cw, ch);
            first_of_frame = have_frame && !frame_vblank;
            if (first_of_frame) frame_vblank = vblank;
            if (first_of_frame && a.valid) {
                /* Alpha 1 must be the game's picture; alpha 0 against the one before. */
                blend_at_t at = {blend, 1, 1, 0, 0};
                size_t k, differ = 0;
                if (!restore(&scratch, a.state.data, a.state.size, a.serial)) return 2;
                replay(&scratch, &j, &at);
                picture(scratch.gpu, drawn, cw, ch);
                ++frames_checked;
                for (k = 0; k < (size_t)cw * ch * 3; k += 3) if (memcmp(drawn + k, now + k, 3)) ++differ;
                if (differ) {
                    ++frames_wrong; wrong_pixels += differ;
                    if (strlen(wrong_list) < sizeof(wrong_list) - 40)
                        snprintf(wrong_list + strlen(wrong_list), sizeof(wrong_list) - strlen(wrong_list), " alpha1@%" PRIu64 ":%zu", vblank, differ);
                }
                if (interval_ready && have_before) {
                    blend_at_t zero = {blend, 0, 1, 0, 0};
                    size_t same = 1;
                    if (!restore(&scratch, a.state.data, a.state.size, a.serial)) return 2;
                    replay(&scratch, &j, &zero);
                    picture(scratch.gpu, start, cw, ch);
                    for (k = 0; k < (size_t)cw * ch; ++k) {
                        int change = memcmp(now + k * 3, before + k * 3, 3) != 0;
                        int jump = memcmp(start + k * 3, before + k * 3, 3) != 0;
                        if (jump) same = 0;
                        if (!in_window) continue;
                        changed += change; jumped += jump;
                        if (jump) ++jumps[k]; else if (change) ++smooth[k];
                    }
                    frames_zero_same += same;
                    if (in_window) ++game_frames;
                    if (verbose && in_window) {
                        size_t c0 = 0, j0 = 0;
                        for (k = 0; k < (size_t)cw * ch; ++k) {
                            c0 += memcmp(now + k * 3, before + k * 3, 3) != 0;
                            j0 += memcmp(start + k * 3, before + k * 3, 3) != 0;
                        }
                        printf("pixels vblank=%" PRIu64 " changed=%zu jumped=%zu\n", vblank, c0, j0);
                    }
                } else if (in_window && have_before) {
                    /* Not blended: everything that changes, changes at once. */
                    for (k = 0; k < (size_t)cw * ch; ++k)
                        if (memcmp(now + k * 3, before + k * 3, 3)) { ++changed; ++jumped; ++jumps[k]; }
                    ++game_frames;
                }
            }
            if (in_window) {
                const uint8_t *shown = now;
                memcpy(last, now, (size_t)cw * ch * 3);
                if (interval_ready && frame_vblank) {
                    /* One game frame late, on the interval measured between the
                     * last two: alpha reaches 1 when the next is due. */
                    uint64_t step = vblank - frame_vblank + 1;
                    blend_at_t at = {blend, (uint32_t)(step < interval ? step : interval), (uint32_t)interval, 0, 0};
                    if (step > interval) ++holds;
                    last_step = step;
                    if (!restore(&scratch, a.state.data, a.state.size, a.serial)) return 2;
                    replay(&scratch, &j, &at);
                    picture(scratch.gpu, drawn, cw, ch);
                    shown = drawn;
                    ++renders;
                    stray += at.stray; split += at.split;
                    if (at.stray) {
                        ++stray_frames;
                        if (strlen(stray_list) < sizeof(stray_list) - 40)
                            snprintf(stray_list + strlen(stray_list), sizeof(stray_list) - strlen(stray_list), " %" PRIu64 ":%u", vblank, at.stray);
                    }
                    if (sheet && !have_sheet && out && frame_vblank >= sheet) {
                        char name[64];
                        if (step == 1) {
                            write_ppm(out, "sheet-before.ppm", before, cw, ch);
                            write_ppm(out, "sheet-start.ppm", start, cw, ch);
                        }
                        snprintf(name, sizeof(name), "sheet-on-%u.ppm", (unsigned)step);
                        write_ppm(out, name, drawn, cw, ch);
                        if (step == interval) { write_ppm(out, "sheet-game.ppm", now, cw, ch); have_sheet = 1; }
                    }
                }
                if (off_file && (fwrite(now, 1, (size_t)cw * ch * 3, off_file) != (size_t)cw * ch * 3 ||
                                 fwrite(shown, 1, (size_t)cw * ch * 3, on_file) != (size_t)cw * ch * 3)) {
                    fprintf(stderr, "Cannot write frames\n"); return 2;
                }
            }
            have_before = 1;
            apply(&truth, in.tag, p, size, NULL);
            if (!journal(&j, in.tag, p, size)) return 2;
            continue;
        } else if (in.tag == FRAME_RECORD_END) break;
        apply(&truth, in.tag, p, size, NULL);
        if (in.tag != FRAME_RECORD_SNAP && !journal(&j, in.tag, p, size)) { fprintf(stderr, "Out of memory\n"); return 2; }
    }
    if (out) {
        uint8_t *heat = malloc((size_t)cw * ch * 3);
        size_t k;
        if (!heat) return 2;
        for (k = 0; k < (size_t)cw * ch; ++k) {
            /* Dimmed grey where nothing changed; where something did, more
             * colour the more often, red for the share of its changes that
             * came at game frames and green for the share that moved between. */
            double grey = (last[k * 3] * 3.0 + last[k * 3 + 1] * 6.0 + last[k * 3 + 2]) / 30.0 * 0.6;
            double total = (double)jumps[k] + smooth[k], a = game_frames ? 4.0 * total / (double)game_frames : 0, r;
            if (a > 1) a = 1;
            r = total > 0 ? jumps[k] / total : 0;
            heat[k * 3] = (uint8_t)(grey * (1 - a) + a * 255 * r + 0.5);
            heat[k * 3 + 1] = (uint8_t)(grey * (1 - a) + a * 255 * (1 - r) + 0.5);
            heat[k * 3 + 2] = (uint8_t)(grey * (1 - a) + 0.5);
        }
        if (!write_ppm(out, "heat.ppm", heat, cw, ch)) failed = 1;
        free(heat);
        if (fclose(off_file) || fclose(on_file)) failed = 1;
    }
    failed |= picture_wrong || frames_wrong;
    printf("Pictures: replayed=%" PRIu64 " differ=%" PRIu64 "\n", pictures, picture_wrong);
    printf("Alpha 1: game-frames=%" PRIu64 " differ=%" PRIu64 " pixels=%" PRIu64 "\n", frames_checked, frames_wrong, wrong_pixels);
    printf("Alpha 0: game-frames-equal-to-previous=%" PRIu64 "\n", frames_zero_same);
    printf("Window: vblanks=%" PRIu64 "-%" PRIu64 " game-frames=%" PRIu64 " renders=%" PRIu64 " holds=%" PRIu64 " skips=%" PRIu64 "\n",
           from, until == UINT64_MAX ? vblank : until, frames, renders, holds, skips);
    printf("Blended: corners3d=%" PRIu64 " moving=%" PRIu64 " (%.2f%%) polygons3d=%" PRIu64 " moving=%" PRIu64 " (%.2f%%)"
           " held-service=%" PRIu64 " held-projection=%" PRIu64 " held-neighbour=%" PRIu64 " held-reset=%" PRIu64
           " held-jump=%" PRIu64 "\n",
           corners_3d, corners_moving, corners_3d ? 100.0 * corners_moving / corners_3d : 0.0, polygons_3d, polygons_moving,
           polygons_3d ? 100.0 * polygons_moving / polygons_3d : 0.0, held_service, held_projection, held_neighbour, held_reset,
           held_jump);
    printf("Game rate: changed=%" PRIu64 " jumped=%" PRIu64 " (%.2f%% of changed, %.2f%% of the screen per game frame)\n",
           changed, jumped, changed ? 100.0 * jumped / changed : 0.0,
           game_frames ? 100.0 * jumped / ((double)game_frames * cw * ch) : 0.0);
    printf("Resets:");
    for (i = 1; i < FRAME_BLEND_REASONS; ++i) printf(" %s=%" PRIu64, reasons[i], reset[i]);
    printf("\nReset at:%s\n", reset_list);
    printf("Jumps (median paired screen move of 32 pixels or more): held=%" PRIu64 " blended=%" PRIu64 " at:%s\n",
           jumps_held, jumps_blended, jump_list);
    printf("Stray: corners=%" PRIu64 " renders=%" PRIu64 " at:%s\n", stray, stray_frames, stray_list);
    printf("Split packets: %" PRIu64 "\n", split);
    printf("Wrong:%s\n", wrong_list);
    frame_record_close(&in);
    frame_record_frame_free(&frame);
    frame_blend_destroy(blend);
    return failed ? 1 : 0;
}
