# Frames on the world map

Status: enabled on this branch after its identity check. Two-frame terrain continuation fixes
the main crack, but visual acceptance still fails: faces absent from both endpoints remain missing. Map-edge wrapping, flight and sea animation remain unverified.

The fifth stage of presenting frames between the game's own: FF8's world map. Frame identity
(`docs/frame-identity.md`) pairs a projected vertex with the vertex of the same transform run a game frame earlier,
by the run's exact model-space inputs. The world map moves its terrain by the camera on the CPU, so no run of
terrain keeps its inputs from one game frame to the next, and until this stage the world map was excluded: shown as
the game shows it. This stage pairs its terrain, checks the pairs against the game's own record of the camera, and,
the check passed, publishes the world map to blending (`docs/frame-blend.md`) and to the player
(`docs/frame-present.md`). Nothing here changes what the game computes.

## How FF8 draws the world map

Observations from the owned-game recordings on the routes below. This page contains no game assets.

- **Game frames** come every 2 VBlanks (about 30 a second), now and then every 3.
- **Terrain** is drawn from blocks of 2048 units square. The CPU adds one offset, the camera's, to every terrain
  point and projects it through RTPT and RTPS under a zero translation and the camera's rotation: 3,000 to 5,600
  points a game frame, drawn as some 800 to 1,000 textured Gouraud triangles from the ordering table the CPU built
  before the display start changed (they link to the frame before). The offset stays small: when the camera crosses
  into another block, the origin of its block (kept by the game, a multiple of 2048) moves by 2048 and the offset back
  by as much. Turning the camera (L1, R1) changes only the rotation; the high view (R2) tilts it and shortens the
  projection distance H over a dozen game frames.
- **Blocks come into view** at the far edge as the camera moves or turns: 250 to 400 points a game frame whose
  places the frame before did not project. Where two blocks meet, both keep the points of their common edge.
  The CPU projects only the terrain it draws: what leaves the screen is not projected again.
- **Far terrain is kept twice on some rays from the camera**: a place and a copy of it 1.2 to 2.4 times as far
  stand on the same pixel while the camera only turns, and part as it moves.
- **Block bounds**: a grid of about 580 points (block corners and their tops) under the camera's rotation and a
  translation, projected to choose which blocks to draw. No polygon is drawn from them.
- **Models** (Squall, the train, the Garden, the rental car, other vehicles) are drawn as in fields and battle: each
  part under its own rotation and translation, so their runs keep their inputs and pair by key.
- **The sky** is two Gouraud quads drawn across the top of the picture, and **the minimap** (a globe on the first
  disc, a flat map later), its marker and the other 2D on top are drawn without projection: they change at the game's
  rate.
- **Transitions.** An encounter ends the world map's frames and the battle's opening effect follows more than 8
  VBlanks later; entering a town or the Garden fades the picture through black frames and loads a field; leaving a
  field for the world map loads through black frames too; the menu changes FF8's mode word.

## Pairing terrain

The host marks the world map's frames with `FRAME_IDENTITY_POINTS` in its classification. In such a frame, after runs
are paired by key, the service pairs the points under a zero translation that are still held:

- **The step.** Three consecutive points of a run make a shape that a common move does not change. A shape found
  once in each frame votes for the move between its two places; the step most shapes agree on, by at least 8 votes
  and more than half of them, is the frame's (`frame_identity_frame_t`'s `step` and `step_votes`, not in the stream).
- **Pairs.** A point pairs with the point of the frame before at its input less the step: the same place in the
  world. Several points there in the frame before (one place in several records, an edge both blocks keep) pair only
  if they stood at one place in view space.
- **Placed points.** A point whose place the frame before has no record of (a block coming into view) is placed
  (`FRAME_IDENTITY_PLACED`): it stood at its input less the step, under the setup the run's paired points stood under
  then, where the GTE's own equations put it. The service links its corners and blending draws it from there, so a
  block that comes in moves with the world from its first frame instead of holding the terrain around it. A place the
  frame before kept at two view positions stays held.
- **Corners where places share a pixel.** Where points of more than one place stand at a polygon corner (a far copy on
  the same ray, distant places on one pixel), the corner takes the place clearly nearest, in the world, to the
  polygon's other linked corners (at most half as far as any other); only if none is does the usual rule (projections
  that stood within a pixel of each other before are one vertex) decide.

Why this way: the vote needs nothing of the game, and the check below finds it exact on every frame. The game's camera
record could be the step itself, but then the check that holds the pairs to it would be the mechanism; it stays the
judge.

## The check

The world map was excluded until a check of its own passed: pairs wrong in at most 0.1% of the decided ones, by the
interpreter's provenance oracle where it can decide and by the camera where it cannot, with what neither decides small
and explained.

- **The camera.** `backends/host/ff8_ram_layout.h` names the world offset (`FF8_WORLD_OFFSET`, x, y and z in RAM) and the
  origin of the camera's block (`FF8_WORLD_ORIGIN`, x and z in the scratchpad); their sum is where the camera stood,
  and its change between two game frames is how far every terrain point's input moved. The host reads them as each
  frame ends. With `--frame-identity` the summary adds, for frames with a step, `step-same`, `step-other` (the voted
  step is, or is not, the camera's change), `step-none`, and for pairs of points under a zero translation
  `place-same` and `place-other` (one place in the world, or two).
- **The oracle** (`--frame-oracle`, `runtime/frame_oracle.c`) is given the camera's change before it judges each frame
  (`frame_oracle_world`). Where its provenance leaves a pair of such points undecided or partial, the camera decides
  it (`place-correct`, `place-wrong`); where provenance finds another source, the camera tells another record of the
  same place (`wrong-same-place`, harmless: the point is drawn where it was) from another place. A placed point is
  right where the frame's step is the camera's change (`placed`, `placed-wrong`), and to judge a link to it the oracle
  computes where it stood on screen. It also counts linked corners whose pair is wrong (`corner-pair-wrong`) or that
  neither can decide (`corner-pair-undecided`).

Wrong pairs are those provenance finds wrong at another place plus those the camera finds at another place.
`test_frame_identity_routes` holds the routes that cross the world map to it in the `game` tier.

| Route | World frames | Steps the camera's | Pairs | Decided | Wrong | Another record, same place | Linked corners on pairs neither decides |
|---|---:|---:|---:|---:|---:|---:|---:|
| magic (walk to an encounter) | 72 | 70 of 70 | 306,527 | 288,622 | 0 | 1,057 | 728 (0.35%) |
| world-walk | 249 | 247 of 247 | 993,620 | 940,065 | 0 | 3,226 | 2,064 (0.37%) |
| world-town | 160 | 157 of 157 | 640,912 | 597,075 | 0 | 4,103 | 1,166 (0.36%) |
| world-car | 529 | 527 of 527 | 1,503,846 | 1,359,681 | 0 | 11,265 | 2,566 (0.30%) |

Every pair of terrain points is one place by the camera (place-other 0 on every route), and no placed point stands on
a wrong step. What provenance cannot decide besides terrain is almost all the block-bounds grid, whose points no
polygon is drawn from (on every route, no drawn corner stands at one of them); of the corners drawn, 0.3 to 0.4% stand
on a pair neither decides. Wrong links (a corner given a projection other than the packet's and that did not move with
it) are 8, 78, 15 and 12 on the four routes, 0.014% of the links at most.

The identity table was rerun on the integrated tree (`ada27b0`) on 2026-11-12, with the oracle host
built in this checkout. Across the four routes: zero wrong pairs out of 3,185,443 decided, zero
wrong placed points, and all 1,001 voted steps equal the camera's change. Each route's service-off,
service-on and oracle runs have one identical final-state hash. The 0.30–0.37% of linked corners
whose pair remains undecided is separate from that zero-wrong count.

## Blending

For terrain with reliable endpoints and an unchanged material, the intermediate mesh is the union of
both game frames' triangles. A triangle is drawn only while its intermediate signed screen area has
the winding of the packet that supplied it. A triangle omitted in the newer frame because it collapses
or turns away can therefore survive until its neighbours close over it. A newly arriving triangle is
the other half of this rule: its presence in the newest packet list does not make its back face visible
earlier in the interval. At alpha 1, only the newest recorded commands are drawn, unchanged.

Existing terrain points follow their view-space endpoints. A point the newer frame no longer projects
is reconstructed from its older input plus the agreed step, under the setup used for the rest of its
run in the newer frame. This includes changes in camera rotation and projection distance H. The older
endpoint needs a reliable projection, not a match to a third, still older frame. Ambiguous outgoing
corners use the same nearest-world-point margin as terrain identification. Missing setup mappings,
incompatible projection modes, near-plane clipping and missing history reject continuation. A synthetic
rotation and H-change case checks closure through 257 presentation fractions; route captures cover
turns, height changes and new blocks.

An outgoing triangle is inserted before the first current polygon, after the clear, in its older draw
order. Current terrain and objects cover it; it cannot paint over a nearer opaque current polygon.
Continuations keep their relative older order. A private depth check of the three recordings found no
visible farther continuation over a nearer continuation, across 8,431,793 overlapping pixel comparisons.
This is evidence for these routes, not a proof for every terrain arrangement.

The continued packet keeps its older UVs, texture page, palette, colour and Gouraud shading: it has no
newer material sample to blend with. Interior continuation is rejected after a write to a sampled
texture or palette tile, a texture-window or relevant drawing-mode change, or for a semitransparent
packet. Revision tracking includes uploads, copies, fills and render-target writes. Its 16-pixel tiles
can conservatively reject a usable material. The existing screen-edge fallback remains when the
interior rule is rejected. Neither rule can supply a triangle omitted from both endpoint packet lists.

Held terrain remains held. Independently releasing corners can separate copies of the same physical
point: one follows a rigid turn, another a straight view-space path. Even with a common straight path,
releasing holds exposes further faces absent from both endpoint lists. Those experiments were not
applied; the interpolation placement functions and identity pairing are unchanged.

## Resets

The world map resets as other scenes do (`docs/frame-blend.md`): an encounter, entering a town or the Garden, and
leaving a field for the world map all pass through more than 8 VBlanks without a game frame; the menu changes the
host's classification. Now and then a game frame on the world map comes more than 8 VBlanks after the one before
(twice on the town route), and is shown as the game shows it. A frame with too few shapes to agree on a step pairs no
terrain by a step, and its terrain stays where the newer frame has it.

## Routes and results

| Route | Card | What it exercises |
|---|---|---|
| `fps60-world-walk.txt` | disc1-early | on foot by Galbadia Garden: camera turns (L1, R1), walking up while turning, left, the high view (R2) while walking down and turning, right, until an encounter |
| `fps60-world-town.txt` | disc1-early | into Galbadia Garden, back out to the world map, the menu opened and closed, a walk |
| `fps60-world-car.txt` | disc3-start | a rental car driven around the Lunar Gate, the camera following |
| `battle-magic.txt` | disc1-early | a walk on the world map to an encounter, then the battle |

Drawn again offline (`inbetween --causes`) from recordings of the three world routes, over windows of 12 to 20
seconds. At alpha 1 every recorded game frame is drawn again pixel for pixel on every route. Final states are the
same with the service off, on, recording, and in the oracle host.

| Window | 3D corners moving | Changed screen still at the game's rate | Resets |
|---|---:|---:|---|
| walk, 1848–2600 (to the encounter's first frames) | 81.75% | 25.20% (14.2% of the screen a game frame) | 1848: initial history; 2384: the encounter |
| walk, world map only, 1848–2380 | 85.2% | 7.8% (4.9%) | 1848: initial history |
| town, 1900–3100 | 92.00% | 34.46% (13.3%) | 2114, 3023: a frame more than 8 VBlanks late; 2254, 2336: into the Garden; 2896: the menu closed |
| car, 6560–7560 | 94.19% | 12.96% (7.7%) | none |

What still changes only at game frames: on foot, almost all of it (94%) is the minimap's globe and the other 2D;
after the encounter starts, its opening effect. On the town route, the globe and the Garden's own backgrounds. On the
car route, the minimap and 2D (53%), far terrain that blending holds where its neighbours did not pair (28%) and far
terrain whose shading changes with distance (16%).

The automatic checks on the complete recordings report zero stray corners and zero resized world
polygons. The 27 resized polygons on the town route remain in the Garden at VBlank 2426.

## Endpoint ground net

Some faces are absent from both game frames but face the camera between them. A union of submitted
faces cannot supply their material. Between world frames, the presenter draws the newer frame's
opaque ground at its recorded positions underneath the interpolated picture. This net supplies ground
where the intermediate mesh opens. It does not invent packets for the missing faces. The live presenter
and offline tool restore polygon serials after injected draws, and restore draw mode and texture window.
Triangles and quads, textured or untextured, flat or Gouraud are supported. Models with their own
translation and semitransparent polygons are excluded. At alpha 0 and 1 the net draws nothing.

The minimap can cover a hole with a semitransparent draw. The hole counter then credits the minimap,
so its clear-colour definition cannot see that hole. The net also closes these holes, including their
dark blended edges. The tool reports net pixels separately by their last opaque fragment. Translucent overlays preserve
that attribution. A later opaque draw replaces it even when the colour does not change.
The hole definition and reporting threshold are unchanged.

The accepted cost is ground extending one to three pixels beyond the blended horizon at the right
screen edge: six grey pixels across VBlanks 6708 and 6744 become slightly lighter. This is invisible
in motion. No timing or cost measurement is claimed.

## Verification status

The endpoint net closes the recorded world holes. Unvisited locations and the outstanding routes
still require verification. The camera check proves terrain displacement on the recorded routes.
It does not prove visibility, texture continuity, or geometry at unvisited locations.

The late cards have not yet been used to establish a flying route. Sea animation, shadows, vehicle exit
and map-edge wrapping remain unverified. The local shape vote alone is not evidence that wrapping
is handled or that it resets safely.

## Limits

Before the net, 202 geometric hole pixels remained in 17 world pictures: faces absent from both
endpoint lists, including a face beyond the screen edge at the newer endpoint. Ten material detections
were also counted. The net now covers these gaps with recorded ground. The Garden remains a field
matter, outside this pass.

Removing holds independently separates shared points at car 7138: identical endpoints at (40,189)
produce (40,189) along one path and (40,190) along the other. Two diverging shared edges explain its
66 new hole pixels. A common straight path removes that fault, but releasing holds still opens 30 new
pixels at 7202: two faces omitted at both endpoints, each signed area -2 there and +29/+30 between.
That variant has 236 car detections instead of 205. Boundary holds introduce different holes, while
historical face retention lacks a proven topology/material lifetime and cannot recover a packet before
it was first submitted. These variants stay outside production.

Far held terrain, distance-dependent shading, the minimap and other 2D still change at the game's rate.
Sea animation, flight, vehicle exit and map-edge wrapping/reset remain unverified. The optional routes
remain outstanding.

## Headless live evidence

At native resolution, fresh live software captures equal the final offline output on all 532 walking
pictures and all 1,000 car pictures: zero differing pixels, including the last picture of each window.
The comparison crops the offline 320 by 240 canvas to live 320 by 224 (rows 8 through 231) and aligns
the same presentation interval. The earlier comparison covered only 531/999 pictures because its
shorter offline window omitted the last one. This establishes picture equality, not display pacing.

No timing or display-pacing claim is made for this continuation change.

## Visual defects observed

The net closes the previously enumerated world geometry holes, including those obscured by the
minimap. Full-recording counts follow. Town's remaining detections are unchanged Garden pixels.

| Route | Hole pixels | Net pixels | Pictures showing net |
|---|---:|---:|---:|
| walk | 0 | 0 | 0 |
| town | 76 | 7 | 1 |
| car | 0 | 263 | 40 |

All alpha-1 frames are exact. Moving corners on the clip windows remain 81.75%, 92.00%, and 94.19%
for walk, town, and car. Stray corners and resized world polygons remain zero.

The net adds three native cases covering omitted-face ground, serial preservation and zero-net
picture equality, model and translucency exclusion, and endpoint suppression. Disabling only the net
fails the omitted-face fixture with 55 incorrect pixels. The final non-game, non-realtime suite has 4,155 passes and two skips.
The earlier battle-magic and field-bedroom windows retain byte-identical output pictures.

Reproduce the counts with `build/devtools/inbetween RECORDING --causes`. Run the native suite with
`FF8_PLAYER_HOST=$PWD/build/player/lionheart SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy nice -n 19 ctest --test-dir build/native-tests -j4 -LE "game|realtime"`.

## Suite receipts

The terrain-continuation tree's standard Linux suite selected 4,154 tests: 4,152 passed and two were
skipped, using the default player and dummy SDL video/audio drivers. A fresh run on 2026-11-13
reproduced these counts with `ctest -LE "game|realtime" -j4`. The game tier was not rerun.
Nine new tests
cover collapse, reversed/incoming facing, an older endpoint without earlier history, rotation plus H
changes, and texture, palette, window and render-target invalidation. The synthetic collapsing patch
has 12,000 clear pixels without the change and zero with it across all 257 fractions. The old incoming
visibility path also fails with 142 visibility errors.
The omitted cases report an empty GTE public-vector parameter set and an unavailable
`VK_LAYER_KHRONOS_validation`, respectively.
Before this continuation change, the complete owned-game tier passed all 25 tests with no skips
(2,120.90 seconds), including native/
interpreter picture and state equality and the separately built oracle and module hosts.
Windows and realtime pacing are outside this verification.

The complete world recordings, including the portions outside the clip windows, also reproduce every
checked game frame at alpha 1: walk 342, town 328 and car 528 (1,198 total), with zero differing pixels.
The three routes' nine delivery videos were regenerated as H.264 at 60 fps. Normal-speed durations are
12.55 seconds (walk), 20 seconds (town) and 16.683 seconds (car); the quarter-speed excerpts are 10.05
seconds each. The older quarter-speed exports were 9.983 seconds and fell just below the requested minimum.

## Earlier scenes

All ten regenerated battle and field window reports are byte-identical to the corresponding trunk
reports. The four battle clips' visual metrics also match their earlier numbers files; the replayed
picture totals differ because the recordings cover different lengths, and the newer tool prints layer
and slide diagnostics. The six field clips predate the trunk's motion and edge-continuation changes:

| Field | Moving 3D corners, old clip / integrated tree | Changed screen at game rate, old / integrated |
|---|---:|---:|
| prison | 83.29% / 85.60% | 6.01% / 5.46% |
| bedroom | 96.46% / 97.08% | 96.16% / 95.50% |
| corridor | 85.83% / 91.80% | 16.22% / 15.99% |
| ring | 55.42% / 58.64% | 30.09% / 30.03% |
| lobby | 74.17% / 78.13% | 16.32% / 16.21% |
| courtyard | 81.05% / 86.12% | 9.77% / 9.28% |

Those differences are already present in the trunk baseline, including the bedroom's increased hole
counter (37 to 439 pixels); they are not introduced by world-map pairing. Alpha 1 has zero differing
pixels in every regenerated window. This comparison does not turn the older scenes' existing visual
defects into passing quality checks.

On all three world routes, fresh recordings are byte-identical to the recordings used for the clips,
and final states are byte-identical with recording, without recording, and with Smooth motion enabled.
The original clips were generated after the terrain-edge continuation change; they were regenerated
here to correct their durations and refresh the contact sheets, not to conceal different runtime output.

All ten reference routes also finish byte-identically with Smooth motion off, on, and against their
reference states: d1-edea, d2-battle, d2-start, d3-start, d3-late, d4-start, d4-last, d4-triad,
magic and summon. Together with the three new world routes, this establishes state equality for the
recorded verification set; it does not cover unvisited world locations.
