#include "frame_record.h"
#include "gpu.h"
#if defined(FF8_SAVESTATES)
#include "savestate_internal.h"
#endif
#include <stdlib.h>
#include <string.h>

static const char magic[8] = {'L', 'H', 'R', 'E', 'C', 'R', 'D', '1'};

/* Bytes. */
static void put(frame_record_bytes_t *b, const void *bytes, size_t n) {
    if (b->failed) return;
    if (b->size + n > b->capacity) {
        size_t c = b->capacity ? b->capacity : 1 << 16;
        uint8_t *p;
        while (c < b->size + n) c *= 2;
        if (!(p = realloc(b->data, c))) { b->failed = 1; return; }
        b->data = p; b->capacity = c;
    }
    memcpy(b->data + b->size, bytes, n);
    b->size += n;
}
static void put32(frame_record_bytes_t *b, uint32_t v) {
    uint8_t w[4] = {(uint8_t)v, (uint8_t)(v >> 8), (uint8_t)(v >> 16), (uint8_t)(v >> 24)};
    put(b, w, 4);
}
static void put64(frame_record_bytes_t *b, uint64_t v) { put32(b, (uint32_t)v); put32(b, (uint32_t)(v >> 32)); }
static void put_le(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}
static uint32_t get32(const uint8_t *p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
static uint64_t get64(const uint8_t *p) { return get32(p) | (uint64_t)get32(p + 4) << 32; }

void frame_record_bytes_free(frame_record_bytes_t *b) {
    free(b->data);
    memset(b, 0, sizeof(*b));
}

void frame_record_serialize(frame_record_bytes_t *b, const frame_identity_frame_t *f) {
    uint32_t i, k;
    put32(b, FRAME_RECORD_FRAM);
    put64(b, f->number); put64(b, f->vblank); put64(b, f->first_projection); put64(b, f->first_polygon);
    put32(b, f->display_start); put32(b, f->tag); put32(b, f->flags);
    put32(b, f->setup_count); put32(b, f->run_count); put32(b, f->projection_count); put32(b, f->polygon_count);
    for (i = 0; i < f->setup_count; ++i) {
        const frame_identity_setup_t *s = &f->setups[i];
        for (k = 0; k < 5; ++k) put32(b, s->rotation[k]);
        for (k = 0; k < 3; ++k) put32(b, (uint32_t)s->translation[k]);
        put32(b, (uint32_t)s->ofx); put32(b, (uint32_t)s->ofy); put32(b, s->h); put32(b, s->shift); put32(b, s->lm);
    }
    for (i = 0; i < f->run_count; ++i) {
        const frame_identity_run_t *r = &f->runs[i];
        put32(b, r->first); put32(b, r->count); put32(b, r->setup); put32(b, (uint32_t)r->previous);
        put32(b, (uint32_t)r->match); put32(b, r->repeats); put64(b, r->key);
    }
    for (i = 0; i < f->projection_count; ++i) {
        const frame_identity_projection_t *p = &f->projections[i];
        put32(b, (uint16_t)p->input[0] | (uint32_t)(uint16_t)p->input[1] << 16);
        put32(b, (uint16_t)p->input[2] | (uint32_t)p->sz << 16);
        put32(b, p->sxy);
        for (k = 0; k < 3; ++k) put32(b, (uint32_t)p->view[k]);
        put32(b, p->run); put32(b, (uint32_t)p->previous); put32(b, (uint32_t)p->match);
    }
    for (i = 0; i < f->polygon_count; ++i) {
        const frame_identity_polygon_t *q = &f->polygons[i];
        put32(b, q->opcode); put32(b, q->corners);
        for (k = 0; k < 4; ++k) {
            put32(b, q->xy[k]); put32(b, (uint32_t)q->state[k] | (uint32_t)q->age[k] << 8);
            put32(b, (uint32_t)q->projection[k]);
        }
    }
}

uint64_t frame_record_picture(gpu_t *g) {
    int x, y, w, h, rgb24, enabled, i, j;
    const uint16_t *vram = gpu_vram(g);
    uint64_t d = UINT64_C(0xcbf29ce484222325);
    int geometry[6];
    gpu_display(g, &x, &y, &w, &h, &rgb24, &enabled);
    geometry[0] = x; geometry[1] = y; geometry[2] = w; geometry[3] = h; geometry[4] = rgb24; geometry[5] = enabled;
    for (i = 0; i < 6; ++i) d = (d ^ (uint32_t)geometry[i]) * UINT64_C(0x100000001b3);
    if (!enabled) return d;
    /* The words the host reads to present it: in 24-bit mode, 1.5 per pixel. */
    if (rgb24) w = (w * 3 + 1) / 2;
    for (j = 0; j < h; ++j)
        for (i = 0; i < w; ++i)
            d = (d ^ vram[((unsigned)(y + j) & 511) * 1024 + ((unsigned)(x + i) & 1023)]) * UINT64_C(0x100000001b3);
    return d;
}

/* Recording. */
static struct {
    int running, failed, recording, done;
    FILE *file;
    gpu_t *gpu;
    uint64_t from, until, vblanks;
    frame_record_bytes_t gp0, chunk;
} r;

static void write_chunk(uint32_t tag, const void *payload, size_t size) {
    uint8_t head[8];
    if (!r.file || r.failed) return;
    if (size > UINT32_MAX) { r.failed = 1; return; }
    put_le(head, tag); put_le(head + 4, (uint32_t)size);
    if (fwrite(head, 1, 8, r.file) != 8 || (size && fwrite(payload, 1, size, r.file) != size)) r.failed = 1;
}

/* GP0 words arrive one at a time from the CPU's port and in blocks from DMA;
 * consecutive ones go into one chunk. */
static void flush_gp0(void) {
    if (!r.gp0.size) return;
    if (r.gp0.failed) r.failed = 1;
    write_chunk(FRAME_RECORD_GP0, r.gp0.data, r.gp0.size);
    r.gp0.size = 0;
}

static void snapshot(void) {
#if defined(FF8_SAVESTATES)
    ss_io io;
    frame_identity_totals_t t = frame_identity_totals();
    flush_gp0();
    memset(&io, 0, sizeof(io));
    ss_gpu(&io, r.gpu);
    r.chunk.size = 0;
    put64(&r.chunk, r.vblanks); put64(&r.chunk, t.polygons); put64(&r.chunk, t.frames);
    put(&r.chunk, io.data, io.size);
    if (io.failed || r.chunk.failed) r.failed = 1;
    else write_chunk(FRAME_RECORD_SNAP, r.chunk.data, r.chunk.size);
    free(io.data);
#else
    r.failed = 1;
#endif
}

static void tap_gp0(void *context, const uint8_t *words, unsigned count) {
    (void)context;
    if (r.recording) put(&r.gp0, words, (size_t)count * 4);
}
static void tap_gp1(void *context, uint32_t word) {
    (void)context;
    if (!r.recording) return;
    flush_gp0();
    {
        uint8_t w[4];
        put_le(w, word);
        write_chunk(FRAME_RECORD_GP1, w, 4);
    }
}
static void tap_read(void *context) {
    (void)context;
    if (!r.recording) return;
    flush_gp0();
    write_chunk(FRAME_RECORD_READ, NULL, 0);
}
static void tap_vblank(void *context) {
    (void)context;
    ++r.vblanks;
    if (!r.recording && !r.done && r.vblanks >= r.from) {
        /* The state before this VBlank; the replay starts by giving it. */
        snapshot();
        r.recording = 1;
    }
    if (!r.recording) return;
    flush_gp0();
    r.chunk.size = 0;
    put64(&r.chunk, r.vblanks); put64(&r.chunk, frame_record_picture(r.gpu));
    write_chunk(FRAME_RECORD_VBLK, r.chunk.data, r.chunk.size);
    if (r.until && r.vblanks >= r.until) {
        write_chunk(FRAME_RECORD_END, NULL, 0);
        r.recording = 0; r.done = 1;
        if (fflush(r.file)) r.failed = 1;
    }
}
static const gpu_tap_t tap = {tap_gp0, tap_gp1, tap_read, tap_vblank};

int frame_record_start(gpu_t *gpu, const char *path, uint64_t from, uint64_t until) {
    frame_record_stop();
    memset(&r, 0, sizeof(r));
#if !defined(FF8_SAVESTATES)
    (void)gpu; (void)path; (void)from; (void)until;
    return 0;
#else
    if (!(r.file = fopen(path, "wb"))) return 0;
    if (setvbuf(r.file, NULL, _IOFBF, 1 << 20) || fwrite(magic, 1, 8, r.file) != 8) r.failed = 1;
    r.from = from; r.until = until; r.gpu = gpu; r.running = 1;
    gpu_tap(gpu, &tap, NULL);
    return 1;
#endif
}

void frame_record_frame(const frame_identity_frame_t *f) {
    if (!r.recording) return;
    flush_gp0();
    r.chunk.size = 0;
    frame_record_serialize(&r.chunk, f);
    if (r.chunk.failed) r.failed = 1;
    /* The payload is the frame without its own tag, which the chunk carries. */
    else write_chunk(FRAME_RECORD_FRAM, r.chunk.data + 4, r.chunk.size - 4);
}

void frame_record_restored(gpu_t *gpu) {
    if (!r.running) return;
    if (r.gpu && r.gpu != gpu) gpu_tap(r.gpu, NULL, NULL);
    r.gpu = gpu;
    gpu_tap(gpu, &tap, NULL);
    if (r.recording) snapshot();
}

int frame_record_stop(void) {
    int ok = 1;
    if (!r.running) return 1;
    if (r.gpu) gpu_tap(r.gpu, NULL, NULL);
    if (r.recording) { flush_gp0(); write_chunk(FRAME_RECORD_END, NULL, 0); }
    if (r.file && fclose(r.file)) r.failed = 1;
    ok = !r.failed;
    frame_record_bytes_free(&r.gp0);
    frame_record_bytes_free(&r.chunk);
    memset(&r, 0, sizeof(r));
    return ok;
}

int frame_record_running(void) { return r.running; }

/* Reading. */
int frame_record_open(frame_record_reader_t *in, const char *path) {
    char head[8];
    memset(in, 0, sizeof(*in));
    if (!(in->file = fopen(path, "rb"))) return 0;
    if (fread(head, 1, 8, in->file) != 8 || memcmp(head, magic, 8)) { fclose(in->file); in->file = NULL; return 0; }
    return 1;
}

int frame_record_next(frame_record_reader_t *in) {
    uint8_t head[8];
    size_t size;
    if (!in->file || fread(head, 1, 8, in->file) != 8) return 0;
    in->tag = get32(head);
    size = get32(head + 4);
    if (size > in->capacity) {
        uint8_t *p = realloc(in->payload, size);
        if (!p) return 0;
        in->payload = p; in->capacity = size;
    }
    if (size && fread(in->payload, 1, size, in->file) != size) return 0;
    in->size = size;
    return 1;
}

void frame_record_close(frame_record_reader_t *in) {
    if (in->file) fclose(in->file);
    free(in->payload);
    memset(in, 0, sizeof(*in));
}

static int reserve(void **items, size_t *capacity, size_t n, size_t size) {
    void *p;
    if (n <= *capacity) return 1;
    if (!(p = realloc(*items, n * size))) return 0;
    *items = p; *capacity = n;
    return 1;
}

int frame_record_parse(frame_record_frame_t *out, const uint8_t *p, size_t size) {
    frame_identity_frame_t *f = &out->frame;
    const uint8_t *end = p + size;
    uint32_t i, k;
    if (size < 4 * 8 + 4 * 7) return 0;
    memset(f, 0, sizeof(*f));
    f->number = get64(p); f->vblank = get64(p + 8); f->first_projection = get64(p + 16); f->first_polygon = get64(p + 24);
    p += 32;
    f->display_start = get32(p); f->tag = get32(p + 4); f->flags = get32(p + 8);
    f->setup_count = get32(p + 12); f->run_count = get32(p + 16);
    f->projection_count = get32(p + 20); f->polygon_count = get32(p + 24);
    p += 28;
    if ((uint64_t)f->setup_count * 52 + (uint64_t)f->run_count * 32 + (uint64_t)f->projection_count * 36 +
        (uint64_t)f->polygon_count * 56 != (uint64_t)(end - p)) return 0;
    if (!reserve((void **)&out->setups, &out->setup_capacity, f->setup_count, sizeof(*out->setups)) ||
        !reserve((void **)&out->runs, &out->run_capacity, f->run_count, sizeof(*out->runs)) ||
        !reserve((void **)&out->projections, &out->projection_capacity, f->projection_count, sizeof(*out->projections)) ||
        !reserve((void **)&out->polygons, &out->polygon_capacity, f->polygon_count, sizeof(*out->polygons))) return 0;
    for (i = 0; i < f->setup_count; ++i, p += 52) {
        frame_identity_setup_t *s = &out->setups[i];
        for (k = 0; k < 5; ++k) s->rotation[k] = get32(p + k * 4);
        for (k = 0; k < 3; ++k) s->translation[k] = (int32_t)get32(p + 20 + k * 4);
        s->ofx = (int32_t)get32(p + 32); s->ofy = (int32_t)get32(p + 36); s->h = get32(p + 40);
        s->shift = get32(p + 44); s->lm = get32(p + 48);
    }
    for (i = 0; i < f->run_count; ++i, p += 32) {
        frame_identity_run_t *r = &out->runs[i];
        r->first = get32(p); r->count = get32(p + 4); r->setup = get32(p + 8); r->previous = (int32_t)get32(p + 12);
        r->match = (int32_t)get32(p + 16); r->repeats = get32(p + 20); r->key = get64(p + 24);
        if ((uint64_t)r->first + r->count > f->projection_count || r->setup >= f->setup_count) return 0;
    }
    for (i = 0; i < f->projection_count; ++i, p += 36) {
        frame_identity_projection_t *q = &out->projections[i];
        uint32_t a = get32(p), b = get32(p + 4);
        q->input[0] = (int16_t)(uint16_t)a; q->input[1] = (int16_t)(uint16_t)(a >> 16);
        q->input[2] = (int16_t)(uint16_t)b; q->sz = (uint16_t)(b >> 16);
        q->sxy = get32(p + 8);
        for (k = 0; k < 3; ++k) q->view[k] = (int32_t)get32(p + 12 + k * 4);
        q->run = get32(p + 24); q->previous = (int32_t)get32(p + 28); q->match = (int32_t)get32(p + 32);
        if (q->run >= f->run_count && f->run_count) return 0;
    }
    for (i = 0; i < f->polygon_count; ++i, p += 56) {
        frame_identity_polygon_t *q = &out->polygons[i];
        q->opcode = get32(p); q->corners = get32(p + 4);
        if (q->corners < 3 || q->corners > 4) return 0;
        for (k = 0; k < 4; ++k) {
            uint32_t sa = get32(p + 12 + k * 12);
            q->xy[k] = get32(p + 8 + k * 12);
            q->state[k] = (uint8_t)sa; q->age[k] = (uint8_t)(sa >> 8);
            q->projection[k] = (int32_t)get32(p + 16 + k * 12);
        }
    }
    f->setups = out->setups; f->runs = out->runs; f->projections = out->projections; f->polygons = out->polygons;
    return 1;
}

void frame_record_frame_free(frame_record_frame_t *f) {
    free(f->setups); free(f->runs); free(f->projections); free(f->polygons);
    memset(f, 0, sizeof(*f));
}
