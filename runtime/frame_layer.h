#ifndef FF8_FRAME_LAYER_H
#define FF8_FRAME_LAYER_H
/* Frame layers: where the 2D layers of a game frame stand at a fraction
 * between that frame and the one before (docs/frame-layer.md).
 *
 * A game that draws a scrolling picture from tiles, as FF8 draws its field
 * backgrounds, keeps the tiles where they are in the picture and moves the
 * GPU's drawing offset (GP0(E5h)) by the scroll; the 3D it draws under that
 * offset is projected into the same picture. Interpolating the offset
 * therefore moves the background, the tiles drawn over the 3D to hide it
 * (masks) and the 3D together, and nothing slides against anything.
 *
 * Each drawing offset a frame sets starts a segment: the GP0 commands after
 * it until the next. A tile is a textured sprite, and it is the same tile in
 * two frames when it draws the same thing: texture page, CLUT, texture
 * coordinates and size. A segment of the newer frame pairs with the segment
 * of the frame before that shares most of its tiles, and its layer is the
 * motion on screen most of its paired tiles share. Tiles that share another
 * motion with enough others are a layer of their own (parallax); a paired tile
 * that moves otherwise, and every rectangle without a texture, stays where the
 * newer frame has it; a tile with no pair (an animated tile drawing something
 * new in its place, or one coming in at the edge) moves with the layer. A
 * segment holds whole, at the newer frame, when the frame has no history, when
 * fewer than half its tiles have a pair (another field, or a fade to another
 * picture), when too many paired tiles move by themselves, and when moving it
 * would uncover the edge of its drawing area that the newer frame covers.
 *
 * The caller hands over each whole GP0 command of a game frame in order, from
 * the display-start change before it to its own, and ends the frame there.
 * Drawing the frame again at a fraction, it counts the frame's offset
 * commands and sprite packets in the same order and asks for each. */
#include <stdint.h>

/* Why a segment does not move. */
enum {
    FRAME_LAYER_MOVED = 0,     /* it moves */
    FRAME_LAYER_NO_HISTORY,    /* no frame before it to move from */
    FRAME_LAYER_STILL,         /* nothing in it moved */
    FRAME_LAYER_CHANGED,       /* fewer than half its tiles have a pair */
    FRAME_LAYER_NOT_RIGID,     /* too many paired tiles move by themselves */
    FRAME_LAYER_EDGE,          /* moving it would uncover its edge */
    FRAME_LAYER_FEW,           /* fewer than FRAME_LAYER_TILES tiles: not a background */
    FRAME_LAYER_REASONS
};

/* A segment with fewer tiles is not taken for a background; a motion that
 * fewer paired tiles share is not a layer of its own. */
enum { FRAME_LAYER_TILES = 16, FRAME_LAYER_PARALLAX = 8, FRAME_LAYER_GROUPS = 4 };

typedef struct {
    uint64_t number;             /* frames ended since the start or a reset */
    uint32_t segments;           /* offset commands in the frame */
    /* The main segment: the one with most tiles. */
    uint32_t reason;             /* FRAME_LAYER_MOVED or why it holds */
    int32_t dx, dy;              /* its layer's move on screen over the interval */
    int32_t offset_dx, offset_dy; /* the change of its drawing offset */
    uint32_t layers;             /* its layers when it moves: 1, or more with layers of their own */
    /* Its sprites: textured ones (tiles), those paired with the frame before
     * that move with a layer, those without a pair that move with the main
     * layer (animated, or new at the edge), those that move by themselves
     * (held), and rectangles without a texture (held). */
    uint32_t tiles, paired, unpaired, disagree, untextured;
    uint32_t moving_segments;    /* segments of the frame that move */
} frame_layer_info_t;

typedef struct frame_layer frame_layer_t;

frame_layer_t *frame_layer_create(void);
void frame_layer_destroy(frame_layer_t *);
/* Forget everything, the GPU state the commands set included. */
void frame_layer_reset(frame_layer_t *);
/* One whole GP0 command of the frame being drawn. Returns 0 if out of memory,
 * after which the frame does not move. */
int frame_layer_command(frame_layer_t *, const uint32_t *words, unsigned count);
/* The frame ends at its display-start change. history is 0 when the frame
 * must not be blended from the one before, whatever its tiles say: the 3D
 * blend found a reset, a change of scene or a cut there (frame_blend.h). */
int frame_layer_end(frame_layer_t *, int history);
const frame_layer_info_t *frame_layer_info(const frame_layer_t *);

/* At alpha = num / den (0 is the frame before, den the latest), the index-th
 * offset command of the latest frame: writes the command to send to word and
 * returns 1 when that differs from the frame's own. */
int frame_layer_offset(const frame_layer_t *, uint32_t index, uint32_t num, uint32_t den, uint32_t *word);
/* The index-th sprite packet of the latest frame at alpha: xy holds its
 * packet's position word and is given the one to send, which keeps it where
 * the newer frame has it on screen unless it moves with a layer. Returns 1
 * when it moves with a layer. */
int frame_layer_sprite(const frame_layer_t *, uint32_t index, uint32_t num, uint32_t den, uint32_t *xy);
/* Whether a segment moves: segment 0 is what the frame draws before its
 * first offset command, segment k + 1 what it draws after the k-th. */
int frame_layer_moves(const frame_layer_t *, uint32_t segment);
/* The main segment's move, as frame_layer_offset would place its origin:
 * the offset at alpha minus the newer one. 0 when it does not move. */
void frame_layer_shift(const frame_layer_t *, uint32_t num, uint32_t den, int32_t *x, int32_t *y);
/* For a renderer that draws finer than the PlayStation's pixels: what
 * frame_layer_offset and frame_layer_sprite round to whole pixels, as a
 * fraction to add to them (the offset's to everything drawn under it, the
 * sprite's to it besides). Both are 0 at the ends of the interval. */
int frame_layer_offset_fraction(const frame_layer_t *, uint32_t index, uint32_t num, uint32_t den, float *x, float *y);
int frame_layer_sprite_fraction(const frame_layer_t *, uint32_t index, uint32_t num, uint32_t den, float *x, float *y);
#endif
