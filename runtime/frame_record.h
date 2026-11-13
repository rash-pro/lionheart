#ifndef FF8_FRAME_RECORD_H
#define FF8_FRAME_RECORD_H
/* A recording of everything needed to draw a stretch of play again away from
 * the game: the GPU's state at the start, every word the guest sent it after
 * that, its VBlanks with a digest of the picture each one showed, and the
 * frame identity service's frames. Replayed through another GPU it gives the
 * same pictures; with the identity frames, a presenter can draw the frames in
 * between (docs/frame-blend.md).
 *
 * The file starts with the eight bytes LHRECRD1, then chunks: a four-byte tag,
 * the payload's length in bytes and the payload, little-endian:
 *   SNAP  VBlank, polygon serial, identity frames delivered (64-bit each),
 *         then the GPU's state as a save state holds it
 *   GP0   words sent to GP0, in order
 *   GP1   one word sent to GP1, before it takes effect
 *   READ  the guest read one word of GPUREAD
 *   VBLK  the VBlank's number and the digest of the picture it showed (64-bit each)
 *   FRAM  one identity frame, as the LHFRAME1 stream holds it (docs/frame-identity.md)
 *   END   the recording window closed
 * VBlanks are counted from the start of the recorder, which starts with the
 * service, so they are the numbers the service gives its frames. Nothing here
 * writes to the guest; it observes through gpu_tap. */
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include "frame_identity.h"

struct gpu;

/* A growing byte buffer. */
typedef struct {
    uint8_t *data;
    size_t size, capacity;
    int failed;
} frame_record_bytes_t;

/* One identity frame as the LHFRAME1 stream holds it, appended to out. */
void frame_record_serialize(frame_record_bytes_t *out, const frame_identity_frame_t *);
void frame_record_bytes_free(frame_record_bytes_t *);

/* What a VBlank showed: the display area's pixels and geometry, as the host
 * presents them. Replays compare against it. */
uint64_t frame_record_picture(struct gpu *);

/* Records VBlanks from..until (inclusive, 0 = no end) to path; the host hands
 * it each identity frame. Needs a build with save states for the GPU's state.
 * Returns 0 and writes nothing when it cannot open the file. */
int frame_record_start(struct gpu *, const char *path, uint64_t from, uint64_t until);
void frame_record_frame(const frame_identity_frame_t *);
/* A state was restored: observe its GPU, and start over from its state. */
void frame_record_restored(struct gpu *);
/* Stop, close the file. Returns 0 if anything failed to write. */
int frame_record_stop(void);
int frame_record_running(void);

/* Reading. */
enum {
    FRAME_RECORD_SNAP = 0x50414e53u, FRAME_RECORD_GP0 = 0x20305047u, FRAME_RECORD_GP1 = 0x20315047u,
    FRAME_RECORD_READ = 0x44414552u, FRAME_RECORD_VBLK = 0x4b4c4256u, FRAME_RECORD_FRAM = 0x4d415246u,
    FRAME_RECORD_END = 0x20444e45u
};

typedef struct {
    FILE *file;
    uint32_t tag;
    uint8_t *payload;
    size_t size, capacity;
} frame_record_reader_t;

/* Opens a recording; 0 if it is not one. */
int frame_record_open(frame_record_reader_t *, const char *path);
/* The next chunk in tag/payload/size; 0 at the end of the file or on a damaged one. */
int frame_record_next(frame_record_reader_t *);
void frame_record_close(frame_record_reader_t *);

/* An identity frame read back: its own copy of everything a sink is given. */
typedef struct {
    frame_identity_frame_t frame;
    frame_identity_setup_t *setups;
    frame_identity_run_t *runs;
    frame_identity_projection_t *projections;
    frame_identity_polygon_t *polygons;
    size_t setup_capacity, run_capacity, projection_capacity, polygon_capacity;
} frame_record_frame_t;

/* Parses an FRAM payload into f, reusing its memory. 0 if it is damaged. */
int frame_record_parse(frame_record_frame_t *f, const uint8_t *payload, size_t size);
void frame_record_frame_free(frame_record_frame_t *);
#endif
