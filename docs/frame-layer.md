# Frame layers

The third stage of presenting frames between the game's own: fields. Frame blending (`docs/frame-blend.md`)
moves 3D between game frames; in a field nearly everything that changes on screen is 2D, the prerendered
background, and it stepped at the game's rate. This stage moves it as a layer, offline in the developer tool that draws
recordings again and live in the player with smooth motion on (`docs/frame-present.md`); it changes nothing
the game computes.

## How FF8 draws a field

What recordings of fields show (the Garden walk and the prison, below); nothing here depends on reading game
data.

- **One drawing offset for the field.** Each game frame sets the drawing area to its back buffer (GP0(E3h),
  GP0(E4h)) and then the drawing offset (GP0(E5h)): the buffer's corner plus where the camera's scroll puts the
  picture. Everything of the field is drawn under that one offset.
- **The background is tiles that stay in the picture.** Under the offset the frame draws the background as
  textured 16x16 sprites (400 to 700 a frame in the fields recorded), some semi-transparent (lights). A tile
  keeps its packet position from frame to frame: scrolling changes only the offset. In the fields recorded the
  scroll is 2 pixels a game frame (the ring hall, horizontally), 6 to 7 (the corridor), 1 pixel every two or
  three frames up to 2 a frame (vertically, the lobby and the courtyard), and both ways at once (the lobby).
- **3D is drawn into the same picture.** The GTE's screen offset (OFX, OFY) stays constant while a field
  scrolls; characters are projected into the picture and the drawing offset moves them with the background.
- **Masks are tiles later in the order.** Tiles and 3D polygons come from one ordering table, sorted by depth:
  a tile drawn after a character covers it. Scenery in front of a character is just such a tile, in the same
  offset as the rest of the background. There is no separate mask.
- **Only the tiles the scroll shows are drawn,** with at least one to spare at each side: as the corridor
  scrolls, 15 to 25 tiles come in and others leave every other frame. On every scrolling frame recorded, the
  tiles of a frame still cover the screen at the offset of the frame before.
- **Animated tiles** draw another part of the texture in the same place (184 tiles every 6 game frames in the
  ring hall, 42 every 4 in the bed room); others keep what they draw and change colour every frame (42 light
  tiles that pulse in the corridor).
- **2D on top has its own offsets.** After the field the frame sets other offsets: one per text window (a save
  point's message), and the buffer's corner for what is drawn in screen space.
- **The fade at a field change** draws no field: each frame copies the picture before from the other buffer
  with one full-screen sprite that subtracts a little from it, and the picture darkens frame by frame.
- **Field changes** pass through black frames while the next field loads: more than 8 VBlanks without a game
  frame, so frame identity resets there.
- **Parallax.** In the courtyard the upper part of the picture (the arch, the trees, the sky), 180 to 310 tiles,
  moves one pixel in the picture against the camera's pan of two or three: a layer that scrolls slower than the
  ground. Every other field recorded scrolls as one layer.

## How it works

`runtime/frame_layer.h` is the whole interface. The caller hands over each whole GP0 command of a game frame in
order (`frame_layer_command`), from the display-start change before it to its own, and ends the frame there
(`frame_layer_end`). Each drawing offset the frame sets starts a **segment**: what it draws until the next
offset. Segment 0 is what it draws before its first offset command.

- **Tiles.** A textured sprite is a tile. Its key is what it draws: texture page (without the semi-transparency
  mode), CLUT, texture coordinates and size. The colour and the blending mode are how it looks (a fade), not which
  tile it is.
- **Pairing segments.** A segment with at least 16 tiles pairs with the segment of the frame before that shares
  most of its keys. With fewer than half of its tiles in that segment, it holds (**changed**: another field, or a
  fade to another picture). Fewer than 16 tiles, and it is not a background (**few**).
- **The layer.** For tiles whose key appears once in each segment, the move on screen (packet position plus
  offset from the drawing area's corner, newer minus older) is counted; the move most share is the segment's
  layer. Another move that 8 or more such tiles share is a layer of its own (up to four).
- **Each tile.** A tile whose pair stands where one of the layers' moves says moves with that layer; a tile with
  no pair (an animated tile drawing something new in its place, or one coming in at the edge) moves with the main
  layer and shows the newer frame's picture; a tile that is one of a kind in both frames but moved otherwise
  stays where the newer frame has it on screen, and so does every rectangle without a texture.
- **Holding.** A segment holds whole, everything where the newer frame has it, when the 3D blend gives the frame
  no history (a reset, a gap, a change of scene, a cut: field-to-field changes are resets), when it is
  **changed** or has **few** tiles, when more than a quarter as many tiles moved by themselves as moved with a
  layer (**not-rigid**), when nothing in it moved (**still**), and at the **edge**: when the newer frame's tiles,
  moved back to any quarter of the interval, would leave uncovered some pixel of the drawing area they cover at
  its end. A rigid move uncovers most at its start, and the quarters catch a hole between tiles.
- **Offsets that follow.** An offset command with nothing drawn under it that sets again the offset of a segment
  that moves moves with it.

At a fraction alpha, `frame_layer_offset` gives each of the latest frame's offset commands, the offset at that
fraction of its change, and `frame_layer_sprite` each sprite's packet position, so that it stands at that
fraction of its layer's move on screen (or, held, where the newer frame has it). Both give exactly the frame's
own at alpha 1. A layer that moves only through the offset keeps every packet as it is, so 3D polygons drawn in
the segment, which frame blending moves in the picture, and the masks drawn over them move with it. Integer
offsets round to the nearest pixel, as blended 3D corners do.

## Drawing again

`tools/devtools/inbetween.c` (`docs/frame-blend.md`) replays each game frame's commands into the layers and, when
it draws a frame at a fraction, asks for its offsets and sprites by order, as it asks frame blending for its
polygons. What is drawn after the display-start change stays as it was. The layers move only where the 3D
blends from the frame before. It adds to its report:

- `Layer`: for game frames whose main segment (the one with most tiles) has 16 tiles or more, how many moved and
  why the others held, frames with a layer of their own, and the main segments' tiles: paired, without a pair,
  moved by themselves, rectangles without a texture.
- `Slide`: 3D corners that move under an offset whose layer holds while the main layer moves. Such corners would
  slide against the background; every route gives 0.
- Among the causes, `sprite-moved-look-changed`: a sprite that moves with its layer where the picture still
  changes only at game frames, because what it draws changed (an animated tile, a light that pulses). `sprite` is
  now a sprite that stays where the newer frame has it.
- With `--frames`, a `layer` line per game frame.

## Routes and results

`tests/scripts/fps60-fields-garden.txt` walks through the Garden from the first save of `disc1-start-maxed.mcr`: the
infirmary bed room, the corridor, the ring hall, the lobby, the turnstiles, the front gate and the courtyard;
`play-disc2-prison.txt` walks across the prison field. `tests/native/cases/test_frame_layer.cpp` checks the
layers on a synthetic field and, with your own discs, that recording both routes changes no final state, that
every game frame drawn again at alpha 1 is the game's picture, that layers move, and that nothing slides.

Measured over each field's window (VBlanks), 3D corners that move and, of the screen area that changes across a
game frame, the share that still changes only at game frames, without layers (frame blending alone) and with
them:

| Field (VBlanks) | Scroll | 3D corners moving | Jumping, blending alone | Jumping, with layers | Frames whose layer moves |
|---|---|---:|---:|---:|---:|
| bed room (1840-2440) | none; 42 animated tiles | 96.5% | 99.1% | 96.2% | 1 of 241 |
| corridor (2880-3480) | horizontal, 6-7 px | 85.8% | 99.5% | 16.2% | 84 of 286 |
| ring hall (3660-4860) | horizontal, 1-2 px; 184 animated tiles | 55.4% | 99.9% | 30.1% | 85 of 586 |
| lobby (5030-5930) | vertical, then both ways | 74.2% | 99.6% | 16.3% | 152 of 416 |
| courtyard (8090-9100) | vertical, with parallax | 81.1% | 95.3% | 9.8% | 84 of 491, 51 of them with two layers |
| prison (13150-13950) | small, both ways | 83.3% | 99.0% | 6.0% | 44 of 400 |

As a share of the whole screen per game frame, what jumps falls from 21.6% to 3.5% in the corridor, 29.2% to
4.8% in the lobby, 13.6% to 4.1% in the ring hall, 12.2% to 1.3% in the courtyard and 9.4% to 0.6% in the
prison. No frame of these windows held at the edge, none was found not rigid, and no 3D corner slid. The battle
routes of frame blending give the same report as before, number for number: battle sets no offset that moves.

What still changes only at game frames:

- **Animated tiles and palettes.** Their pictures change at the game's own rate (every 4 to 6 game frames); in
  fields where the camera does not move they are most of what changes at all. They are not motion, and they are
  drawn as the newer frame draws them.
- **3D that frame blending holds**: characters' corners the service holds (unpaired or ambiguous projections),
  and polygons held with them. See below.
- **Text windows, the fade** and anything else drawn in screen space change at the game's rate, as before.

## Why the ring hall moved few 3D corners

Frame blending moved 47% of the ring hall's 3D corners while it scrolled and 52% while it did not: the scroll
does not change identity (the GTE's offsets stay constant; the scroll is the drawing offset). The ring hall shows
several students far below the camera. Their models project joint vertices one at a time, each under its own
bone, with the same model-space input (for example the origin of the bone) for many bones: repeated keys, which
pair only with a clearly nearest candidate within 8 pixels. On models a few pixels tall, every candidate is that
close, so they are held, and each such vertex is a corner of many polygons. Of the corners held at an unpaired
projection of their own frame, two in three are these repeated single vertices; the rest are mostly vertices
the CPU moves into place itself, whose inputs change every frame. Holding a corner holds its polygon and the surface
around it. Pairing repeated keys by their order within a model, rather than by distance, would recover them; that
is a change to frame identity, which battle uses too, and it is left for later.

## Limits

- Offsets and sprite positions are whole pixels: a scroll of one pixel every few game frames steps a pixel at a
  time, as the game's does.
- A layer must move rigidly. A background whose tiles move by themselves holds; a fade drawn by changing tile
  colours moves with its layer and changes its look at the game's rate.
- The edge check holds a frame rather than drawing tiles the newer frame no longer draws; no recorded frame
  needed it.
- Layers of their own are found per frame from the tiles they share; a parallax layer of fewer than 8 tiles
  whose keys are unique holds instead.
