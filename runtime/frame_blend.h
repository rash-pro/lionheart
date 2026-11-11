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
 * corner at the position of such a corner on a polygon joined to it by shared
 * edges, so that a moving polygon never leaves a gap against a held one; and
 * whole polygons that would otherwise stretch between held corners and
 * corners that move far.
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

/* A run that turns farther than this many degrees between two game frames
 * moves on straight lines: near half a turn, which way it went is a guess. */
enum { FRAME_BLEND_TURN_LIMIT = 150 };

/* A camera cut: paired runs moved, at the vertex-weighted median, at least
 * FRAME_BLEND_CUT_MOVE / 1000 of their depth in view space, and at least half
 * of that move is not what the velocity they had one frame earlier predicts.
 * With few runs this also holds a single part that jumps that far, which is
 * the safe side. A cut to a distant shot moves runs little against the depth
 * they have after it, but far on screen: a frame whose paired projections
 * moved a median of FRAME_BLEND_CUT_SCREEN pixels or more is a cut from
 * FRAME_BLEND_CUT_NEAR / 1000 of their depth on, under the same rule for the
 * change. */
enum { FRAME_BLEND_CUT_MOVE = 300, FRAME_BLEND_CUT_SCREEN = 32, FRAME_BLEND_CUT_NEAR = 100 };

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
    /* Moving corners that follow their run's rotation rather than a straight
     * line; runs paired with the frame before that turn (of them, those that
     * hang from another run), whose matrix flattens space or mirrors at one
     * end only, and that turn farther than FRAME_BLEND_TURN_LIMIT. */
    uint32_t corners_turning, runs_turning, runs_attached, runs_flat, runs_far;
    /* Corners the service held as ambiguous that the same polygon a game
     * frame earlier resolved. */
    uint32_t corners_resolved;
    /* Polygons whose colours change from the same polygon a game frame
     * earlier (blended where frame_blend_colours allows). */
    uint32_t polygons_recoloured;
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

/* Checks on what frame_blend_polygon draws, added up over calls. */
typedef struct {
    /* Corners off their path by more than two pixels where the GTE saturates
     * nothing: a corner on a straight line off the line between its ends, a
     * corner that turns farther from that line than an arc of at most half a
     * turn and its turn about its part's origin allow. Something drawn
     * visibly wrong. */
    unsigned stray;
    /* Polygons whose every corner moves, and of them those with an edge, in
     * view space, more than a tenth shorter or longer than at both ends: a
     * part that shrinks or grows between two frames that agree on its size. */
    unsigned polygons, resized;
} frame_blend_check_t;

/* A polygon of the latest frame, by serial, at alpha = num / den (0 is the
 * frame before, den the latest): writes the packet words of its corners to xy
 * and returns a mask of the corners that move. 0 when it stays as recorded.
 * check, if given, adds up what it finds drawn wrong. */
unsigned frame_blend_polygon(const frame_blend_t *, uint64_t serial, uint32_t num, uint32_t den,
                             uint32_t xy[4], frame_blend_check_t *check);

/* Each polygon packet the GPU executes, by the serial frame identity counts
 * (docs/frame-identity.md), before the identity frame that holds it: what
 * blending knows of a polygon's look. */
void frame_blend_packet(frame_blend_t *, uint64_t serial, const uint32_t *packet);

/* The corner colours of a polygon of the latest frame at alpha = num / den,
 * between those of the same polygon a game frame earlier and its own: a
 * polygon whose every corner is linked, drawn by the same command, with the
 * same semi-transparency where it is textured. Writes 24-bit colours to
 * colours (one per corner; a flat polygon's are all its one colour) and
 * returns a mask of those that differ from the latest frame's; 0 when its
 * colours stay as recorded. */
unsigned frame_blend_colours(const frame_blend_t *, uint64_t serial, uint32_t num, uint32_t den, uint32_t colours[4]);

/* Move every corner on a straight line in view space, as before runs turned
 * (for comparison); takes effect from the next frame added. */
void frame_blend_straight(frame_blend_t *, int);

/* SXY as the GTE gives it for a view-space position (MAC1-3) under a setup's
 * OFX, OFY, H, sf and lm. */
uint32_t frame_blend_project(const int32_t view[3], int32_t ofx, int32_t ofy, uint32_t h, uint32_t shift, uint32_t lm);
#endif
