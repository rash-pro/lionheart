#include "frame_present_host.h"
#include "frame_identity_host.h"
#include "frame_identity.h"
#include "frame_present.h"
#include "frame_record.h"
#include "hw.h"
#ifdef FF8_PGXP
#include "pgxp.h"
#endif
#ifdef FF8_VULKAN
#include "vulkan_renderer.h"
#endif
#include <SDL.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* What the guest hands the presenter, in order, when another thread draws
 * (host_frame_present_queue): a tag byte, the payload's length, the payload. */
enum { EVENT_GP0, EVENT_GP1, EVENT_READ, EVENT_VBLANK, EVENT_COMMAND, EVENT_CORNERS, EVENT_FRAME, EVENT_SYNC };

typedef struct {
    uint8_t *data;
    size_t size, capacity;
    int failed;
} buffer_t;

static struct {
    frame_present_t *present;
    gpu_t *guest;
    int running, queued;
    int precision;           /* PGXP's corners are taken for the presenter */
    /* The queue: the guest thread appends to incoming; the drawing thread
     * takes it whole and works through it outside the lock. */
    SDL_mutex *lock;
    buffer_t incoming, working;
    frame_record_bytes_t frame;
    frame_record_frame_t parsed;
    void (*rebind)(gpu_t *);
} h;

static void put(buffer_t *b, unsigned tag, const void *a, size_t na, const void *c, size_t nc) {
    size_t need = 5 + na + nc;
    uint32_t size = (uint32_t)(na + nc);
    if (b->failed) return;
    if (b->size + need > b->capacity) {
        size_t n = b->capacity ? b->capacity * 2 : 1 << 20;
        uint8_t *d;
        while (n < b->size + need) n *= 2;
        if (!(d = realloc(b->data, n))) { b->failed = 1; return; }
        b->data = d; b->capacity = n;
    }
    b->data[b->size] = (uint8_t)tag;
    memcpy(b->data + b->size + 1, &size, 4);
    if (na) memcpy(b->data + b->size + 5, a, na);
    if (nc) memcpy(b->data + b->size + 5 + na, c, nc);
    b->size += need;
}

static void event(unsigned tag, const void *a, size_t na, const void *c, size_t nc) {
    SDL_LockMutex(h.lock);
    put(&h.incoming, tag, a, na, c, nc);
    SDL_UnlockMutex(h.lock);
}

static void tap_gp0(void *context, const uint8_t *words, unsigned count) {
    (void)context;
    if (h.queued) event(EVENT_GP0, words, (size_t)count * 4, NULL, 0);
    else frame_present_gp0(h.present, words, count);
}
static void tap_gp1(void *context, uint32_t word) {
    (void)context;
    if (h.queued) event(EVENT_GP1, &word, 4, NULL, 0);
    else frame_present_gp1(h.present, word);
}
static void tap_read(void *context) {
    (void)context;
    if (h.queued) event(EVENT_READ, NULL, 0, NULL, 0);
    else frame_present_read(h.present);
}
static void tap_vblank(void *context) {
    (void)context;
    if (h.queued) event(EVENT_VBLANK, NULL, 0, NULL, 0);
    else frame_present_vblank(h.present);
}
static const gpu_tap_t tap = {tap_gp0, tap_gp1, tap_read, tap_vblank};

static void command(void *context, const uint32_t *packet, unsigned count, uint64_t polygon) {
    (void)context;
    if (h.queued) event(EVENT_COMMAND, &polygon, 8, packet, (size_t)count * 4);
    else frame_present_command(h.present, packet, count, polygon);
#ifdef FF8_PGXP
    /* PGXP resolves a corner on the guest's GPU, which is about to draw it:
     * the presentation GPU, drawing the packet again, cannot ask. */
    if (h.precision && (packet[0] >> 24) >= 0x20 && (packet[0] >> 24) < 0x40) {
        pgxp_vertex_t v[4] = {{0, 0, 0}, {0, 0, 0}, {0, 0, 0}, {0, 0, 0}};
        float corner[4][3];
        unsigned i, mask = pgxp_packet(h.guest, packet, v);
        if (!mask) return;
        for (i = 0; i < 4; ++i) { corner[i][0] = v[i].x; corner[i][1] = v[i].y; corner[i][2] = v[i].w; }
        if (h.queued) {
            uint8_t payload[12 + sizeof(corner)];
            uint32_t m = mask;
            memcpy(payload, &polygon, 8); memcpy(payload + 8, &m, 4); memcpy(payload + 12, corner, sizeof(corner));
            event(EVENT_CORNERS, payload, sizeof(payload), NULL, 0);
        }
        else frame_present_corners(h.present, polygon, mask, corner);
    }
#endif
}

static void sink(void *context, const frame_identity_frame_t *f) {
    int x, y, w, hh, rgb24, enabled;
    (void)context;
    gpu_display(h.guest, &x, &y, &w, &hh, &rgb24, &enabled);
    if (h.queued) {
        int32_t size[2];
        size[0] = w; size[1] = hh;
        h.frame.size = 0;
        frame_record_serialize(&h.frame, f);
        /* Without the four-byte tag the stream gives each frame, as frame_record_parse takes it. */
        if (!h.frame.failed) event(EVENT_FRAME, size, sizeof(size), h.frame.data + 4, h.frame.size - 4);
    } else frame_present_frame(h.present, f, w, hh);
}

#ifdef FF8_VULKAN
static void keep(void *context) { (void)context; host_gpu_checkpoint(frame_present_gpu(h.present), 0); }
static void put_back(void *context) { (void)context; host_gpu_checkpoint(frame_present_gpu(h.present), 1); }
static void between(void *context, int on) { (void)context; host_gpu_between(on); }
#endif

/* The guest's GPU as it stands, handed to the presenter: directly, or as a
 * copy in the queue that the drawing thread takes up in its turn. */
static void sync_with(gpu_t *guest) {
    uint64_t polygon = frame_identity_totals().polygons;
    gpu_t *copy;
    if (!h.queued) { frame_present_sync(h.present, guest, polygon); return; }
    if (!(copy = gpu_create())) return;
    gpu_copy(copy, guest);
    event(EVENT_SYNC, &polygon, 8, &copy, sizeof(copy));
}

static void attach(cpu_t *c) {
    h.guest = c->hw->gpu;
    gpu_tap(h.guest, &tap, NULL);
    frame_identity_rebind(h.guest);
    sync_with(h.guest);
}

int host_frame_present_start(cpu_t *c, int fractions, int precision) {
    frame_identity_config_t config;
    if (h.running) return 1;
    memset(&h, 0, sizeof(h));
    if (frame_identity_running()) {
        fprintf(stderr, "Smooth motion cannot run with the frame identity developer switches\n");
        return 0;
    }
    if (!(h.lock = SDL_CreateMutex())) return 0;
    if (!(h.present = frame_present_create())) { SDL_DestroyMutex(h.lock); h.lock = NULL; return 0; }
    frame_present_fractions(h.present, fractions);
#ifdef FF8_PGXP
    h.precision = fractions ? precision : 0;
#else
    (void)precision;
#endif
    frame_present_precision(h.present, h.precision);
#ifdef FF8_VULKAN
    {
        frame_present_renderer_t renderer = {keep, put_back, between, NULL};
        frame_present_renderer(h.present, &renderer);
        host_gpu_checkpoints(1);
    }
#endif
    memset(&config, 0, sizeof(config));
    config.sink = sink; config.classify = host_frame_identity_classify; config.command = command;
    if (!frame_identity_start(c->hw->gpu, &config)) {
        frame_present_destroy(h.present); SDL_DestroyMutex(h.lock); memset(&h, 0, sizeof(h));
        return 0;
    }
    host_frame_identity_bind(1);
    h.running = 1;
    attach(c);
    return 1;
}

int host_frame_present_running(void) { return h.running; }
gpu_t *host_frame_present_gpu(void) { return h.running ? frame_present_gpu(h.present) : NULL; }

void host_frame_present_queue(int on, void (*rebind)(gpu_t *)) {
    if (!h.running) return;
    SDL_LockMutex(h.lock);
    h.queued = on; h.rebind = rebind;
    SDL_UnlockMutex(h.lock);
}

/* Frees the guest copies a queue still holds. */
static void release(buffer_t *b) {
    size_t i = 0;
    while (i + 5 <= b->size) {
        uint32_t size;
        memcpy(&size, b->data + i + 1, 4);
        if (b->data[i] == EVENT_SYNC) {
            gpu_t *copy;
            memcpy(&copy, b->data + i + 5 + 8, sizeof(copy));
            free(copy);
        }
        i += 5 + size;
    }
    b->size = 0;
}

/* On the drawing thread: everything the guest queued since, in order. */
static void drain(void) {
    size_t i = 0;
    buffer_t t;
    SDL_LockMutex(h.lock);
    t = h.working; h.working = h.incoming; h.incoming = t;
    h.incoming.size = 0; h.incoming.failed = 0;
    SDL_UnlockMutex(h.lock);
    if (h.working.failed) {
        /* Out of memory on the way: what was queued is lost, and so is the
         * history; nothing is blended until the presenter starts over. */
        release(&h.working);
        h.working.failed = 0;
        return;
    }
    while (i + 5 <= h.working.size) {
        unsigned tag = h.working.data[i];
        uint32_t size;
        const uint8_t *p = h.working.data + i + 5;
        memcpy(&size, h.working.data + i + 1, 4);
        switch (tag) {
        case EVENT_GP0: frame_present_gp0(h.present, p, size / 4); break;
        case EVENT_GP1: { uint32_t w; memcpy(&w, p, 4); frame_present_gp1(h.present, w); break; }
        case EVENT_READ: frame_present_read(h.present); break;
        case EVENT_VBLANK: frame_present_vblank(h.present); break;
        case EVENT_COMMAND: {
            uint64_t polygon;
            uint32_t words[12];
            unsigned n = (size - 8) / 4;
            memcpy(&polygon, p, 8);
            if (n > 12) n = 12;
            memcpy(words, p + 8, (size_t)n * 4);
            frame_present_command(h.present, words, n, polygon);
            break;
        }
        case EVENT_CORNERS: {
            uint64_t polygon;
            uint32_t mask;
            float corner[4][3];
            memcpy(&polygon, p, 8); memcpy(&mask, p + 8, 4); memcpy(corner, p + 12, sizeof(corner));
            frame_present_corners(h.present, polygon, mask, corner);
            break;
        }
        case EVENT_FRAME: {
            int32_t wh[2];
            memcpy(wh, p, sizeof(wh));
            if (frame_record_parse(&h.parsed, p + sizeof(wh), size - sizeof(wh)))
                frame_present_frame(h.present, &h.parsed.frame, wh[0], wh[1]);
            break;
        }
        case EVENT_SYNC: {
            uint64_t polygon;
            gpu_t *copy;
            memcpy(&polygon, p, 8);
            memcpy(&copy, p + 8, sizeof(copy));
            frame_present_sync(h.present, copy, polygon);
            free(copy);
            /* The renderer starts again from the new state, here, where it draws. */
            if (h.rebind) h.rebind(frame_present_gpu(h.present));
            break;
        }
        default: break;
        }
        i += 5 + size;
    }
    h.working.size = 0;
}

void host_frame_present_take(void) { if (h.queued) drain(); }

gpu_t *host_frame_present_draw(cpu_t *c, double since, int whole) {
    if (h.queued) drain();
    else if (c && c->hw->gpu != h.guest) host_frame_present_restored(c);
    frame_present_draw(h.present, whole ? -1 : (double)frame_present_vblanks(h.present) + since);
    return frame_present_gpu(h.present);
}

void host_frame_present_restored(cpu_t *c) {
    if (!h.running) return;
    if (h.guest && h.guest != c->hw->gpu) gpu_tap(h.guest, NULL, NULL);
    attach(c);
}

void host_frame_present_stop(int report) {
    if (!h.running) return;
    host_frame_identity_bind(0);
    if (h.guest) gpu_tap(h.guest, NULL, NULL);
#ifdef FF8_VULKAN
    host_gpu_checkpoints(0);
#endif
    frame_identity_stop();
    release(&h.incoming); release(&h.working);
    if (report) {
        frame_present_totals_t t = frame_present_totals(h.present);
        const frame_present_clock_t *k = frame_present_timing(h.present);
        fprintf(stderr, "Smooth motion: draws=%" PRIu64 " blended=%" PRIu64 " between=%" PRIu64 " holds=%" PRIu64
                " skips=%" PRIu64 " advances=%" PRIu64 " settles=%" PRIu64 " syncs=%" PRIu64 " failures=%" PRIu64 "\n",
                t.draws, t.blended, k->between, k->holds, k->skips, t.advances, t.settles, t.syncs, t.failures);
        if (h.precision)
            fprintf(stderr, "Smooth motion corners: blended=%" PRIu64 " pgxp=%" PRIu64 " whole=%" PRIu64 "\n",
                    t.corners_blended, t.corners_pgxp, t.corners_whole);
    }
    frame_present_destroy(h.present);
    free(h.incoming.data); free(h.working.data);
    frame_record_bytes_free(&h.frame); frame_record_frame_free(&h.parsed);
    SDL_DestroyMutex(h.lock);
    memset(&h, 0, sizeof(h));
}
