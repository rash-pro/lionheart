# Frame blending

The second stage of presenting frames between the game's own: drawing them. It builds on frame identity
(`docs/frame-identity.md`), which says for every polygon corner the GPU draws which projected vertex it is and
where that vertex stood a game frame earlier. This stage records play, draws it again away from the game through
the software rasterizer, and draws the frames in between. Nothing here runs in the player unless a developer
switch asks for a recording, and nothing changes what the game computes.

## Recording

```sh
lionheart ... --frame-record play.rec [--frame-record-window FROM:UNTIL]
```

The switch starts the frame identity service and records VBlanks `FROM` to `UNTIL` (1 to the end by default):
the GPU's state as a save state holds it at the first, then every word the guest sends to GP0 and GP1, every
GPUREAD, every VBlank with a digest of the picture it shows (the display area as the host presents it), and every
identity frame in the `LHFRAME1` layout. `runtime/frame_record.h` gives the file layout. The GPU reports its
traffic through `gpu_tap`, which costs one test per call without a tap. A build without save states cannot record.
Recordings hold the game's pictures; keep them out of the repository.

## Blending

`runtime/frame_blend.h`. Given the identity frames in order, it says where each polygon of the latest frame stands
at a fraction alpha between that frame and the one before:

- A corner linked to a projection moves from where the projection's pair stood to where the projection stands.
  Its view-space position (MAC1-3) and the setup's OFX, OFY and H are interpolated as integers and projected again
  with the GTE's own equations (`gte_divide`), so alpha 0 and alpha 1 are exactly the screen positions the game
  computed, and corners that share a packet position share one vertex: nothing cracks. Corners of age 1 are
  projections of the frame before, paired with the frame before that.
- A corner is followed only if both ends are what the GTE's equations give for them, with the same sf and lm,
  and neither end lies at or behind the camera plane, where the GTE's division saturates (SZ3 at most H/2).
- Everything else stays where the newer frame has it: corners the service holds, every corner of a polygon with
  a held corner, every corner at the position of such a corner, and every corner at a position where projections
  that stand together now stood more than a pixel apart before.
- A polygon whose held corners would stay while its other corners move more than 8 pixels over the interval is
  held whole, and with it every position it shares, until no such polygon is left: it would stretch visibly.
- Drawing order is the newer frame's: the game's ordering table is replayed as it was.

A frame gives nothing to blend from after the service's own resets (the first frame, more than 8 VBlanks since the
previous one, an overflow, a restored state), in excluded scenes (the world map), when the host's classification
(FF8's mode word) changes, and at a cut:

- For each transform run paired with the frame before, its move is how far the centre of its vertices moved in
  view space over its depth, and its change how far the centre stands from where the run's velocity one frame
  earlier would have put it (the whole move when there is no such velocity).
- A frame is a cut when the vertex-weighted median move is at least 0.3 and the median change at least half of
  that. A camera that swoops close past a model moves its runs by several times their depth each frame, but
  steadily; a cut moves almost every run by a large part of its depth at once, and nothing before predicts it.
  Erratic camera moves that large (a camera tumbling around a model) are held too.
- A run that meets the same rule on its own is held alone: a model put somewhere else at once, such as an
  attacker placed in front of the camera, while the scene around it moves on.

## Drawing again

`tools/devtools/inbetween.c` (built with the developer tools, `build/devtools/inbetween`):

```sh
inbetween play.rec [--from V] [--until V] [--out DIR] [--canvas WxH] [--sheet V] [--frames]
```

It replays the recording through `runtime/gpu.c` and checks, at every VBlank, that the replay shows what the
game showed (the recorded digest), and for every game frame that drawing it again with every blended polygon at
alpha 1 gives the game's picture pixel for pixel. To draw a game frame at some alpha it restores the GPU state of
the display-start change before the frame's own, replays everything since, with the polygons of the frame where
blending puts them, up to the VBlank shown. Everything else is replayed as it was, so the HUD and other 2D drawn
after the change are those of the VBlank shown.

The timeline is that of a presenter one game frame late, as a live one could run it: the game frame whose display
start changed at VBlank `v` (shown from `v + 1`) is drawn at VBlank `v + j` with alpha `min(j, n) / n`, where `n` is
the number of VBlanks between that change and the one before. When the next frame comes later than `n` the picture
stays at alpha 1 (a hold); when it comes earlier, the rest of the way is skipped (a skip). A frame with nothing to
blend from is shown as the game shows it.

Measures, over `--from` to `--until`:

- `Blended`: 3D corners (those of polygons with a corner at a projection) and polygons that move, and why the rest
  stay: held by the service, ends the GTE does not give again or at the camera plane, in or next to a held polygon,
  no history, a run that jumped by itself.
- `Game rate`: for each game frame, the pixels where alpha 0 differs from the picture shown just before it change
  only at game frames; of the pixels that change across the game frame, their share, and their share of the
  screen. Without blending both shares would be those of every changing pixel.
- `Resets` and their VBlanks; `Jumps`: frames whose paired projections moved a median of 32 pixels or more on
  screen, the measure frame identity's summary uses, held or blended.
- `Stray`: blended corners more than two pixels off the line between their two ends where the GTE saturates
  nothing, which a straight move in view space never does: something drawn wrong.

`--out` writes `off.rgb` and `on.rgb`, raw 24-bit frames of the canvas (320x240 by default), one per VBlank, and
`heat.ppm`: the last picture of the window in dim grey; where pixels changed, the more often the stronger, red for
the share of their changes that jumped at game frames and green for the share that moved between them. `--sheet V` writes the pictures of the first blended game frame from VBlank `V` on.

`tests/native/cases/test_frame_blend.cpp` checks blending and the replay on a synthetic scene, and, with the
owner's discs, that recording changes no route's final state and that every game frame of the recorded windows is
drawn again exactly.
