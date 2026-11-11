# Frame blending

The second stage of presenting frames between the game's own: drawing them. It builds on frame identity
(`docs/frame-identity.md`), which says for every polygon corner the GPU draws which projected vertex it is and
where that vertex stood a game frame earlier. This stage records play, draws it again away from the game through
the software rasterizer, and draws the frames in between. The player draws them live with smooth motion on
(`docs/frame-present.md`); otherwise nothing here runs in it unless a developer switch asks for a recording, and
nothing changes what the game computes.

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
- A corner the service held as ambiguous (several projections at its position that stood apart before: parts
  that cross on screen, a coiled chain drawn a few pixels across) is resolved by the same polygon a game frame
  earlier: the polygon of the frame before with the same command whose linked corners are, corner by corner, the
  pairs of this one's (the game draws a model's polygons from the same list every frame). Its packet word at
  that corner is where the vertex stood, so the projection at this position whose pair stood exactly there is
  the vertex; several that did stood together then and now, and are one vertex. Nothing is resolved when
  several such polygons disagree there.
- A corner whose transform run turns follows the run's motion instead of a straight line. Each paired run's
  matrix, at both ends, is split into a rotation and what stays besides it (scale, shear, a mirror), by the
  polar decomposition; the rotation is interpolated along the shorter arc at constant speed and the rest
  linearly, so a part that turns keeps its size between two game frames (a straight line in view space draws a
  part that turns a quarter turn at 71% of its size halfway). A run whose origin, put in another run's space at
  one end, lands where its origin is at the other (to 2 view units plus 1% of the distance, both ways) hangs from
  that run, as a bone from its parent: its origin is carried by the parent's motion, and it turns relative to the
  parent or on its own, whichever is the smaller turn. A chain of parts that each swing far against the camera (a
  whip-like weapon, a limb) then bends a little at each joint, and its joints stay where both parts put them.
  Runs with no rotation to follow move on straight lines, as before: a matrix that flattens space (a shadow
  squashed onto the floor), one mirrored at one end only, a turn of more than 150 degrees in one game frame,
  where the other way round is nearly as short and which way the part went is a guess (a straight line keeps
  it between its two ends, a wrong guess would draw it where it never was), different vertices at the two
  ends, and a vertex whose arc would reach the camera plane. Alpha 0 and 1 stay the game's own positions.
- A polygon whose every corner is linked takes its corner colours from between those of the same polygon a game
  frame earlier (found as for ambiguous corners) and its own, when both are drawn by the same command and, where
  textured, with the same semi-transparency: lighting that changes as a model turns, flashes and fades. Raw
  textures, which ignore the colour, and a change of blending mode, which cannot be drawn in between, keep the
  recorded colours. Blending knows a polygon's look from `frame_blend_packet`, which the host calls with each
  polygon packet the GPU executes.
- A vertex projected anew each game frame, so with no pair (a shadow's centre: the game computes it from where
  the model stands), follows the polygons around it when it is the one unpaired corner of every polygon that
  uses it, has three neighbours or more, none held, and they all move alike (their moves on screen within 2 pixels
  and a quarter of their mean): it moves by their mean move, so the fan moves as a unit. Otherwise it is held,
  and its polygons with it.
- 3D polygons that share an edge (both its ends at the same packet positions) form one surface, and a position
  is one vertex within a surface. A gap can open only along a shared edge; two surfaces that meet at a single
  position, often by chance (an effect's corner on a vertex of the floor), do not hold each other.
- Everything else stays where the newer frame has it: corners the service holds, every corner of a polygon with
  a held corner, every corner of its surface at the position of such a corner, and every corner at a position
  where projections that stand together now stood more than a pixel apart before.
- A polygon whose held corners would stay while its other corners move more than 8 pixels over the interval is
  held whole, and with it every position of its surface it shares, until no such polygon is left: it would
  stretch visibly. A polygon the GPU draws none of (each of its triangles spans 1024 pixels or more across or
  512 or more down, as one with a corner clamped far off screen does) holds nothing and is drawn as recorded:
  otherwise its clamped corner held its neighbours and, through this rule, a whole floor.
- Drawing order is the newer frame's: the game's ordering table is replayed as it was.
- On a fast camera move, the newer frame no longer draws what the older one had at the edge of the screen: the
  game culls polygons that left it. A polygon of the frame before with no successor in the newer frame (no
  polygon found for it as for ambiguous corners), every corner linked, whose vertices a game frame later all lie
  beyond one edge of the screen, is drawn between the two frames first, beneath everything, where its vertices'
  own motion puts them (the game projects a model's vertices before it culls its polygons), with its own packet;
  `frame_blend_leftover` gives it, and never at alpha 1. A polygon still on the screen that the newer frame no
  longer draws went for another reason (it turned away, or a polygon held in its place), and is not drawn: a
  ghost of it would show. The host tells blending the screen's size (`frame_blend_screen`).

A frame gives nothing to blend from after the service's own resets (the first frame, more than 8 VBlanks since the
previous one, an overflow, a restored state), in excluded scenes (the world map), when the host's classification
(FF8's mode word) changes, and at a cut:

- For each transform run paired with the frame before, its move is how far the centre of its vertices moved in
  view space over its depth, and its change how far the centre stands from where the run's velocity one frame
  earlier would have put it (the whole move when there is no such velocity).
- A frame is a cut when the vertex-weighted median move is at least 0.3 and the median change at least half of
  that. A cut to a distant shot moves runs little against their new depth but far on screen, so a frame
  whose paired projections moved a median of 32 pixels or more on screen is a cut from a move of 0.1 on, with
  the same rule for the change. A camera that swoops close past a model moves its runs by several times their
  depth each frame, but steadily; a cut moves almost every run by a large part of its depth at once, and nothing
  before predicts it.
  Erratic camera moves that large (a camera tumbling around a model) are held too.
- A run that meets the same rule on its own is held alone: a model put somewhere else at once, such as an
  attacker placed in front of the camera, while the scene around it moves on.

## Drawing again

`tools/devtools/inbetween.c` (built with the developer tools, `build/devtools/inbetween`):

```sh
inbetween play.rec [--from V] [--until V] [--out DIR] [--canvas WxH] [--sheet V] [--frames] [--causes] [--packets V] [--straight]
```

It replays the recording through `runtime/gpu.c` and checks, at every VBlank, that the replay shows what the
game showed (the recorded digest), and for every game frame that drawing it again with every blended polygon at
alpha 1 gives the game's picture pixel for pixel; with `--from`, that check starts a few VBlanks before the window
(drawing every game frame again is what takes the time) and the replay ends at `--until`. To draw a game frame at
some alpha it restores the GPU state of the display-start change before the frame's own, replays everything since,
with the polygons of the frame where blending puts them, up to the VBlank shown. Everything else is replayed as it
was, so the HUD and other 2D drawn after the change are those of the VBlank shown.

The timeline is the guest's own: one picture per VBlank (59.94 Hz with NTSC timing), each the newest game frame
blended one game frame late, as a live presenter could run it without seeing the future: the game frame
whose display start changed at VBlank `v` (shown from `v + 1`) is drawn at VBlank `v + j` with alpha
`min(j, n) / n`, where `n` is the number of VBlanks between that change and the one before. When the next frame
comes later than `n` the picture stays at alpha 1 (a hold); when it comes earlier, the rest of the way is skipped
(a skip). Whether a corner moves depends on the identity frame its projection belongs to, not on the one that
draws it: battle draws each game frame from the projections of the frame before, so a cut in a frame's projections
shows a game frame later, and the frame whose projections cut still blends what it draws. Only a game frame that
comes more than 8 VBlanks after the one before, or the first after the recording's state, is shown as the game
shows it. Game frames come every 2 to 6
VBlanks (fields about every 2, battle about every 4); each interval is the one measured before it, so a steady rate
gives evenly spaced steps and a change of rate costs one hold or skip.

Measures, over `--from` to `--until`:

- `Blended`: 3D corners (those of polygons with a corner at a projection) and polygons that move, and why the rest
  stay: held by the service, ends the GTE does not give again or at the camera plane, in or next to a held polygon,
  no history, a run that jumped by itself; the ambiguous corners resolved, and the polygons whose colours change
  from a game frame earlier, and the polygons of the frame before drawn between because the newer frame culled
  them at the screen's edge (`culled-at-edge`), and the corners that follow the polygons around them
  (`following`).
- `Game rate`: for each game frame, the pixels where alpha 0 differs from the picture shown just before it change
  only at game frames; of the pixels that change across the game frame, their share, and their share of the
  screen. Without blending both shares would be those of every changing pixel.
- `Resets` and their VBlanks (of the frames whose projections have nothing to blend from; in battle the picture
  cuts a game frame later); `Jumps`: frames whose paired projections moved a median of 32 pixels or more on
  screen, the measure frame identity's summary uses, held or blended.
- `Turning`: moving corners that follow their run's rotation, runs that turn (and of them, those hanging from
  another), runs whose matrix is flat or mirrored at one end only, and runs that turn too far.
- `Stray`: blended corners off their path by more than two pixels where the GTE saturates nothing: on a straight
  line, off the line between their two ends, which a straight move in view space never does; turning, farther
  from that line than half the distance between the ends plus their distance from the origin of every part that
  carries them, which an arc of at most half a turn never is. Something drawn wrong.
- `Size`: polygons that move whole, and of them those with an edge, in view space, more than a tenth shorter or
  longer than at both ends: a part that shrinks or grows between two frames that agree on its size.
  `--straight` moves every corner on a straight line, for comparison.
- With `--causes`, `Causes`: what drew each pixel that changes only at game frames, as shares of those pixels:
  3D polygons that move but whose colour, texture coordinates, order or what lies under them changed (or whose
  texture or palette was rewritten in the frame); 3D polygons held, by the first reason that applies (a frame
  without history, a corner no projection made, a projection without pair, an ambiguous or split corner, the
  blending rules above); polygons with no corner at a projection; sprites, lines, fills and transfers; what is
  drawn after the display-start change; and game frames not blended at all. Where alpha 0 draws a moving polygon or
  the background, the pixel is blamed on what the picture before had there (something held that left).
- With `--causes`, `Holes`: pixels of the blended pictures left to the clear colour, or not drawn, where both game
  frames had 3D: geometry the newer frame no longer draws (gone past the screen edge in a fast camera move) or a
  crack between a held polygon and a moving one. Renders with 16 or more are listed.

`--out` writes `off.rgb` and `on.rgb`, raw 24-bit frames of the canvas (320x240 by default), one per VBlank, and
`heat.ppm`: the last picture of the window in dim grey; where pixels changed, the more often the stronger, red for
the share of their changes that jumped at game frames and green for the share that moved between them. `--sheet V`
writes the pictures of the first blended game frame from VBlank `V` on. With `--causes`, `causes.ppm` colours each
pixel that jumped by what most often caused it. `--packets V` (which implies `--causes`) prints, for the first
blended game frame from VBlank `V` on, the packets that alpha 0 draws where the picture jumps, with their identity
states, and writes `probe-before.ppm`, `probe-start.ppm`, `probe-game.ppm` and `probe-causes.ppm`.

`tests/native/cases/test_frame_blend.cpp` checks blending, the replay and the measures on a synthetic scene,
and, with your own discs, that recording changes no route's final state and that every game frame of the recorded
windows is drawn again exactly, with no stray corner.

## Limits

The offline renderer here is the reference the player's live frames are checked against
(`docs/frame-present.md`). What still moves only at game frames, by what the measures find on the battle and field routes:

- 2D: sprites, the HUD and menus. Field backgrounds, drawn as tiles under a drawing offset that scrolls with the
  camera, move with that offset as layers, the 3D drawn under it with them (`docs/frame-layer.md`); their
  animated tiles still change at the game's rate.
- 3D the service cannot link: effect geometry projected anew each frame (no pair), polygons whose corners the CPU
  computes without the GTE, and ambiguous corners whose polygon was not drawn the same way a game frame earlier.
  Holding a corner holds its polygon and the surface around it.
- 3D that moves, but whose look changes at game frames in ways blending does not follow: texture coordinates,
  which polygons are drawn, their order, a change of blending mode, or a texture or palette rewritten during the
  frame, as many spell and summon effects do.
- Parts that turn more than 150 degrees within one game frame, and parts whose matrix is no rotation, move on
  straight lines and shrink between the frames.
- The game frame that shows a cut or a change of scene, and the first after a gap, are shown as the game shows them.
  On the battle routes each cut holds one game frame and no more; the few frames the rule holds that are no cut
  (a model filling the screen that spins half a turn in place, a fast pull-back of the camera) cannot be told
  from a spell's run of cuts between close-ups by how far what is on the screen moves.
- An edge of the screen can still show the background for an in-between where the newer frame culled a polygon
  that is still partly on the screen, which is not drawn again for fear of a ghost.
