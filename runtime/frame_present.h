#ifndef FF8_FRAME_PRESENT_H
#define FF8_FRAME_PRESENT_H
/* Frame presentation: the frames between a game's frames, drawn while it runs
 * (docs/frame-present.md).
 *
 * The presenter keeps a GPU of its own, the presentation GPU, that draws what
 * the guest's GPU draws, from the guest's own commands: every GP0 and GP1
 * word, read and VBlank, as a tap gives them. It keeps that GPU's state at the
 * start of the newest game frame (the checkpoint) and the commands since. To
 * show the game at some moment it puts the checkpoint back and draws those
 * commands again, with the newest game frame's polygons where frame blending
 * puts them and its layers where frame layers put them at that moment's
 * fraction of the interval, and everything after the frame's display-start
 * change (the HUD, the next frame's drawing) as it was. At the end of the
 * interval that is exactly the guest's picture. Nothing here touches the
 * guest.
 *
 * A game frame drawn at fraction 1 becomes the next checkpoint on the way;
 * a frame the next one cut short is drawn once more at 1, unseen, to get
 * there. The renderer behind the presentation GPU, when it keeps VRAM of its
 * own, is told when to keep and put back its copy. */
#include <stdint.h>
#include "frame_identity.h"
#include "gpu.h"

/* The phase of the newest game frame: a pure function of when game frames
 * end, kept apart so that it can be checked on its own. A game frame that
 * ends n VBlanks after the one before is drawn from the VBlank after its end
 * at fraction (t - end) / n, where n is the interval measured before it: a
 * steady rate gives evenly spaced steps and a change of rate costs one hold
 * (the fraction stays at 1 until the next frame) or one skip (the next frame
 * comes before 1). A frame more than FRAME_IDENTITY_GAP VBlanks after the
 * one before, or one that cannot be blended, is shown as the game shows it. */
typedef struct {
    uint64_t end, interval;   /* the newest frame's end (VBlank count) and its interval */
    uint64_t frames;          /* frames ended */
    int ready;                /* blended: an interval to go by */
    /* What the steps did: renders at a fraction below 1, at 1 because the
     * next frame was late (holds), and frames cut short by the next (skips). */
    uint64_t between, holds, skips;
    double reached;           /* the furthest fraction the newest frame was drawn at */
} frame_present_clock_t;

void frame_present_clock_reset(frame_present_clock_t *);
/* A game frame ended at VBlank count vblank; blendable is 0 when it must be
 * shown as the game shows it. */
void frame_present_clock_frame(frame_present_clock_t *, uint64_t vblank, int blendable);
/* The fraction at guest time t (VBlank count, with the part of the current
 * VBlank's period that has passed) as num / den. Returns 1 when the newest
 * frame is blended there, 0 when it is shown whole (num = den). */
int frame_present_clock_phase(frame_present_clock_t *, double t, uint32_t *num, uint32_t *den);

typedef struct frame_present frame_present_t;

/* The renderer behind the presentation GPU, when it keeps VRAM of its own:
 * save makes what it holds now the checkpoint, restore puts the checkpoint
 * back; between says whether the drawing that follows is a picture between
 * game frames (1), which nothing is kept from, or a whole one (0). Any may be
 * NULL. */
typedef struct {
    void (*save)(void *context);
    void (*restore)(void *context);
    void (*between)(void *context, int on);
    void *context;
} frame_present_renderer_t;

frame_present_t *frame_present_create(void);
void frame_present_destroy(frame_present_t *);
/* The presentation GPU: what a host shows and its renderer mirrors. */
gpu_t *frame_present_gpu(frame_present_t *);
void frame_present_renderer(frame_present_t *, const frame_present_renderer_t *);
/* Draw finer than whole pixels where frame blending and layers can say so
 * (gpu_subpixel); for renderers above the PlayStation's resolution. */
void frame_present_fractions(frame_present_t *, int);
/* With fractions, PGXP's precision (1 vertex, 2 perspective; 0, the default,
 * none): every 3D corner drawn where the GTE put it before it dropped the
 * fraction, game frames included, and in perspective with its depth.
 * Corners frame blending moves get theirs from the view-space positions
 * frame identity gave; the others from the guest's PGXP, which only the
 * guest's GPU can consult, through frame_present_corners. */
void frame_present_precision(frame_present_t *, int mode);
/* Where the guest's PGXP put the corners of a polygon packet the guest's GPU
 * is about to execute (pgxp_packet), by polygon serial, as frame_present_command
 * numbers it: per corner x and y in packet coordinates and the depth (0 for
 * none), for the corners in mask. */
void frame_present_corners(frame_present_t *, uint64_t polygon, unsigned mask, float corner[4][3]);
/* Start over from the guest's GPU as it stands, with polygon serial polygon
 * (frame_identity_totals): nothing to blend from until two frames end. */
void frame_present_sync(frame_present_t *, gpu_t *guest, uint64_t polygon);

/* What the guest sends its GPU, as a gpu_tap_t gives it. */
void frame_present_gp0(frame_present_t *, const uint8_t *words, unsigned count);
void frame_present_gp1(frame_present_t *, uint32_t word);
void frame_present_read(frame_present_t *);
void frame_present_vblank(frame_present_t *);
/* Each whole GP0 command the guest's GPU executes, with its polygon serial
 * (frame_identity_config_t's command), and each identity frame as it ends,
 * with the size of the picture the guest shows. Returns 0 if out of memory,
 * after which nothing is blended until a sync. */
void frame_present_command(frame_present_t *, const uint32_t *packet, unsigned count, uint64_t polygon);
int frame_present_frame(frame_present_t *, const frame_identity_frame_t *, int32_t width, int32_t height);

/* VBlanks the presenter has seen since the last sync. */
uint64_t frame_present_vblanks(const frame_present_t *);
/* Draws the presentation GPU as the game should be shown at guest time t
 * (VBlank count, see frame_present_clock_phase). A negative t draws the
 * latest state whole. Returns 1 when something moved between game frames. */
int frame_present_draw(frame_present_t *, double t);
const frame_present_clock_t *frame_present_timing(const frame_present_t *);

/* Totals since the presenter was created. */
typedef struct {
    uint64_t draws, blended, advances, settles, syncs, failures;
    uint64_t replayed_words; /* words drawn again, all draws */
    /* With precision, the polygon corners drawn: where blending placed them,
     * where the guest's PGXP did, and at whole pixels (2D, or what PGXP did
     * not resolve). */
    uint64_t corners_blended, corners_pgxp, corners_whole;
} frame_present_totals_t;
frame_present_totals_t frame_present_totals(const frame_present_t *);
#endif
