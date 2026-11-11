#ifndef FF8_FRAME_IDENTITY_H
#define FF8_FRAME_IDENTITY_H
/* Frame identity: which projected vertex of a game frame is which vertex of the
 * frame before, and which projection each drawn polygon corner came from. This
 * is what a presenter needs to draw frames between two game frames.
 *
 * Every projected vertex passes through the GTE's RTPS or RTPT, from
 * interpreted and recompiled code alike. The service records each projection
 * there: the model-space input, the rotation, translation and screen constants
 * it was projected with, and the result. It groups a game frame's projections
 * into transform runs (consecutive projections under one setup, in practice a
 * bone or a rigid part), keys each run by its exact inputs, and pairs it with
 * the run of the same key in the previous game frame; vertex k pairs with
 * vertex k. A key seen more than once in either frame pairs only with the
 * mutually nearest run, clearly nearer than the next and close on screen;
 * otherwise it is held. A game frame ends where the guest changes the display
 * start (GP1(05h)), and more than FRAME_IDENTITY_GAP VBlanks between two ends
 * leaves the new frame without history.
 *
 * Nothing here writes to the guest, and nothing runs unless a host starts the
 * service and binds the recording entries below in place of gte_rtps and
 * gte_rtpt: in lh_api for modules, in interp_gte_command for the interpreter.
 * The unbound path is the normal one, unchanged. One service per process, as
 * one host GPU per process. */
#include <stddef.h>
#include <stdint.h>
#include "cpu.h"

struct gpu;

enum { FRAME_IDENTITY_HELD = -1, FRAME_IDENTITY_GAP = 8 };

/* Frame flags. */
#define FRAME_IDENTITY_RESET 1u    /* no history: the first frame, a gap, a reset or an overflow before */
#define FRAME_IDENTITY_EXCLUDED 2u /* the host excluded it: nothing is paired for use */
#define FRAME_IDENTITY_OVERFLOW 4u /* more records than the service keeps: nothing is paired */

/* A host classification with this bit excludes the frame. */
#define FRAME_IDENTITY_EXCLUDE 0x80000000u
/* With this bit the frame's scene moves some of its points on the CPU, by a
 * step common to all of them, and projects them under a zero translation: a
 * world drawn around a camera. Such points also pair with the point of the
 * frame before at their input less that step (see frame_identity_step). */
#define FRAME_IDENTITY_POINTS 0x40000000u

/* What a projection was computed with, as the GTE held it. */
typedef struct {
    uint32_t rotation[5];      /* control 0-4, packed as the GTE keeps them */
    int32_t translation[3];    /* control 5-7 */
    int32_t ofx, ofy;          /* control 24-25 */
    uint32_t h;                /* control 26 */
    uint32_t shift, lm;        /* the command's sf (0 or 12) and lm */
} frame_identity_setup_t;

typedef struct {
    int16_t input[3];          /* model-space vertex V */
    uint16_t sz;               /* screen depth SZ */
    uint32_t sxy;              /* SXY as the GTE stores it: x low, y high */
    /* View-space position (TR * 4096 + RT * V) >> sf, what MAC1-3 hold for
     * this vertex when nothing overflows. */
    int32_t view[3];
    uint32_t run;              /* its transform run in this frame */
    /* The vertex it is in the previous frame, by index in that frame's
     * projections, or FRAME_IDENTITY_HELD. Held in reset, excluded and
     * overflowing frames. */
    int32_t previous;
    /* What the mechanism matched, also in excluded frames: for measurement
     * only, never for presentation. */
    int32_t match;
} frame_identity_projection_t;

typedef struct {
    uint32_t first, count;     /* its projections */
    uint32_t setup;            /* index in the frame's setups */
    int32_t previous;          /* the run it pairs with, or FRAME_IDENTITY_HELD */
    int32_t match;             /* as projection.match */
    uint32_t repeats;          /* other runs with the same key in this frame */
    uint64_t key;
} frame_identity_run_t;

/* Why a corner is held, or that it is linked. */
enum {
    FRAME_IDENTITY_LINKED = 0,     /* its projection is known and paired */
    FRAME_IDENTITY_NO_PROJECTION,  /* no projection is at this position: 2D */
    FRAME_IDENTITY_SPLIT,          /* no one frame holds every corner of the polygon */
    FRAME_IDENTITY_UNPAIRED,       /* a projection at this position has no pair */
    FRAME_IDENTITY_AMBIGUOUS,      /* projections at this position were apart before */
    FRAME_IDENTITY_NO_HISTORY,     /* its frame was reset or overflowed */
    FRAME_IDENTITY_EXCLUDED_CORNER /* it would be linked, but its frame is excluded */
};

typedef struct {
    uint32_t opcode;           /* GP0 command, 0x20-0x3f */
    uint32_t corners;          /* 3 or 4 */
    uint32_t xy[4];            /* each corner's packet word */
    uint8_t state[4];          /* FRAME_IDENTITY_LINKED or why it is held */
    /* The projection a corner came from: in this frame (age 0) or the one
     * before (age 1), by index in that frame's projections. Present for linked
     * and excluded corners, FRAME_IDENTITY_HELD otherwise. */
    uint8_t age[4];
    int32_t projection[4];
} frame_identity_polygon_t;

/* One game frame: the projections made since the display start last changed,
 * and the polygons drawn in that time. Polygons drawn after the change that
 * follows their projections, the common case for 3D drawn from the ordering
 * table the CPU built before the swap, link to the previous frame (age 1).
 * Everything here is valid only during the sink call. */
typedef struct {
    uint64_t number;           /* game frames since the service started */
    uint64_t vblank;           /* the VBlank count when the display start changed */
    uint64_t first_projection; /* serial of projections[0] since the service started */
    uint64_t first_polygon;    /* serial of polygons[0] */
    uint32_t display_start;    /* the start this frame changed away from */
    uint32_t tag;              /* the host's classification, see frame_identity_config_t */
    uint32_t flags;
    /* With FRAME_IDENTITY_POINTS: the step the CPU moved this frame's points
     * by since the frame before, and how many of its shapes voted for it (0:
     * none was found, and no point paired by it). Not in the stream. */
    int32_t step[3];
    uint32_t step_votes;
    uint32_t setup_count, run_count, projection_count, polygon_count;
    const frame_identity_setup_t *setups;
    const frame_identity_run_t *runs;
    const frame_identity_projection_t *projections;
    const frame_identity_polygon_t *polygons;
} frame_identity_frame_t;

typedef struct {
    /* Receives each game frame as it ends. */
    void (*sink)(void *context, const frame_identity_frame_t *frame);
    /* Optional: called as a frame ends with the CPU that last projected (NULL
     * before any); the result is stored as the frame's tag, and with
     * FRAME_IDENTITY_EXCLUDE set the frame is excluded. */
    uint32_t (*classify)(void *context, const cpu_t *c);
    /* Optional: each whole GP0 command the observed GPU executes, in order,
     * with the serial its polygon has or, for any other command, the serial
     * the next polygon will have (what a presenter needs besides the frames:
     * drawing offsets, sprites, every polygon's look). */
    void (*command)(void *context, const uint32_t *packet, unsigned count, uint64_t polygon);
    void *context;
    /* Repeated keys pair only within this many pixels on screen; 0 means 8. */
    unsigned repeat_pixels;
} frame_identity_config_t;

/* Observe this GPU and deliver frames to the sink. The host binds the entries
 * below while the service runs. Returns 0 if out of memory. */
int frame_identity_start(struct gpu *, const frame_identity_config_t *);
/* Observe another GPU (one restored from a state) and drop all history. */
void frame_identity_rebind(struct gpu *);
/* Drop history: the next frame starts without one. */
void frame_identity_reset(void);
/* Stop observing, without delivering the frame in progress, and free memory. */
void frame_identity_stop(void);
int frame_identity_running(void);

/* Recording entries, the same contract as gte_rtps, gte_rtpt and gte_cmd. */
void frame_identity_rtps(cpu_t *c, unsigned shift, int lm);
void frame_identity_rtpt(cpu_t *c, unsigned shift, int lm);
void frame_identity_gte_command(cpu_t *c, uint32_t opcode);

/* Totals since the service started. */
typedef struct {
    uint64_t frames, projections, polygons;
} frame_identity_totals_t;
frame_identity_totals_t frame_identity_totals(void);
#endif
