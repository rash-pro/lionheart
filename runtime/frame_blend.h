#ifndef FF8_FRAME_BLEND_H
#define FF8_FRAME_BLEND_H
/* Frame blending: where each polygon of a game frame stands at a fraction
 * between that frame and the one before, from what the frame identity service
 * says about them (docs/frame-blend.md).
 *
 * A polygon corner the service linked to a projection moves from where that
 * projection's vertex stood in the frame before to where it stands now. Its
 * view-space position (the GTE's MAC1-3) is interpolated and projected again
 * with the GTE's own equations, so a vertex shared by several polygons gets
 * one position and nothing cracks, and the two ends of the interval are
 * exactly the screen positions the game computed. Everything else stays where
 * the newer frame has it: corners of polygons with any corner held, and every
 * corner at the position of such a corner, so that a moving polygon never
 * leaves a gap against a held one; and whole polygons that would otherwise
 * stretch between held corners and corners that move far.
 *
 * A frame gives no history to blend from after the service's own resets
 * (the first frame, a gap, an overflow, a restored state), in excluded
 * scenes, when the scene changes, and at camera cuts: frames where most of the
 * paired vertices jumped in view space by a large part of their depth, in a
 * way their motion before did not predict. A transform run that does so by
 * itself is held alone. */
#include <stdint.h>
#include "frame_identity.h"

/* Why a frame has nothing to blend from. */
enum {
    FRAME_BLEND_KEPT = 0,     /* blended */
    FRAME_BLEND_NO_HISTORY,   /* the service gave it none, or the frame before was not seen */
    FRAME_BLEND_EXCLUDED,     /* an excluded scene */
    FRAME_BLEND_SCENE,        /* the host's classification changed */
    FRAME_BLEND_CUT,          /* most paired vertices jumped: a camera cut */
    FRAME_BLEND_REASONS
};

/* A polygon whose held corners would stay while others move more than this
 * many pixels over the interval is held whole: it would visibly stretch. */
enum { FRAME_BLEND_STRETCH = 8 };

/* A camera cut: paired runs moved, at the vertex-weighted median, at least
 * FRAME_BLEND_CUT_MOVE / 1000 of their depth in view space, and at least half
 * of that move is not what the velocity they had one frame earlier predicts.
 * With few runs this also holds a single part that jumps that far, which is
 * the safe side. */
enum { FRAME_BLEND_CUT_MOVE = 300 };

typedef struct {
    uint64_t number, vblank;      /* the identity frame's */
    uint64_t first_polygon;
    uint32_t polygon_count;
    uint32_t reason;              /* FRAME_BLEND_KEPT or why not */
    uint32_t move, change;        /* the cut measures in thousandths, 0 without pairs */
    /* The median screen move of paired projections in pixels (the larger
     * axis), the measure frame identity's summary counts jumps by. */
    uint32_t screen_move;
    /* Polygons with a corner at a projection (3D), their corners, the corners
     * that move and the polygons whose every corner moves. */
    uint32_t polygons_3d, corners_3d, corners_moving, polygons_moving;
    /* 3D corners kept at the newer frame: held by the service; linked, but
     * with ends the GTE's equations do not give again or at the camera plane,
     * where its division saturates; linked, but in a
     * polygon with a held corner or at the position of one (or of a corner
     * that stood elsewhere before); in a frame without history; or in a
     * transform run that jumped by itself, by the cut rule applied to it alone. */
    uint32_t held_service, held_projection, held_neighbour, held_reset, held_jump;
} frame_blend_info_t;

typedef struct frame_blend frame_blend_t;

frame_blend_t *frame_blend_create(void);
void frame_blend_destroy(frame_blend_t *);
/* Forget everything: the next frame has no history. */
void frame_blend_reset(frame_blend_t *);
/* Each identity frame in order, as the service delivers it. Returns 0 if out
 * of memory, after which the frame is not blended. */
int frame_blend_add(frame_blend_t *, const frame_identity_frame_t *);
/* What the latest frame added gives. */
const frame_blend_info_t *frame_blend_info(const frame_blend_t *);

/* A polygon of the latest frame, by serial, at alpha = num / den (0 is the
 * frame before, den the latest): writes the packet words of its corners to xy
 * and returns a mask of the corners that move. 0 when it stays as recorded.
 * stray, if given, counts corners that left the line between their two ends
 * by more than two pixels, where the GTE saturates nothing: something drawn
 * visibly wrong. */
unsigned frame_blend_polygon(const frame_blend_t *, uint64_t serial, uint32_t num, uint32_t den,
                             uint32_t xy[4], unsigned *stray);

/* SXY as the GTE gives it for a view-space position (MAC1-3) under a setup's
 * OFX, OFY, H, sf and lm. */
uint32_t frame_blend_project(const int32_t view[3], int32_t ofx, int32_t ofy, uint32_t h, uint32_t shift, uint32_t lm);
#endif
