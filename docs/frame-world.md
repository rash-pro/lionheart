# Frames on the world map

Status: enabled on this branch after its identity check, but visual acceptance fails because
of terrain cracks. Map-edge wrapping, flight and sea animation remain unverified.

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

Blending (`docs/frame-blend.md`) draws paired and placed terrain points on a straight line in view space between
their two places, and the models as in fields and battle. Terrain that leaves the screen has no projection a game
frame later, since the CPU stops projecting it; a polygon of the frame before that the newer frame no longer draws,
every corner linked, is drawn beneath everything between the two (`culled-at-edge`) when a corner of it stands off
the screen a game frame later, its points where the step puts them: their input plus the step under the setup the
newer frame projected the rest of their run under. Without it the edge the camera moves away from showed the clear
colour, up to 200 pixels a picture along the top on the car route.

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
| walk, 1848–2600 (to the encounter's first frames) | 81.8% | 25.2% (14.2% of the screen a game frame) | 1848: initial history; 2384: the encounter |
| walk, world map only, 1848–2380 | 85.2% | 7.8% (4.9%) | 1848: initial history |
| town, 1900–3100 | 92.0% | 34.5% (13.3%) | 2114, 3023: a frame more than 8 VBlanks late; 2254, 2336: into the Garden; 2896: the menu closed |
| car, 6560–7560 | 94.2% | 13.0% (7.7%) | none |

What still changes only at game frames: on foot, almost all of it (94%) is the minimap's globe and the other 2D;
after the encounter starts, its opening effect. On the town route, the globe and the Garden's own backgrounds. On the
car route, the minimap and 2D (53%), far terrain that blending holds where its neighbours did not pair (28%) and far
terrain whose shading changes with distance (16%).

The tool's automatic checks: no stray corners and no polygon that grows or shrinks on the world map (the 27 resized
on the town route are in the Garden). Pixels left to the clear colour: 5 on foot and 110 on the town route, below the tool's
per-picture reporting threshold; these counts are not zero. The car route has 821 pixels in 52 of
1,001 pictures; 20 pictures meet the default 16-pixel reporting threshold. The visible crack is one
pixel high across a row of terrain (below).


## Verification status

The identity check and visual acceptance are separate gates. The world map is enabled on this branch,
but the visible car crack fails the no-new-wrong-pictures gate. It must not be reported as release-ready.
The camera check proves terrain displacement on the recorded routes; it does not prove visibility,
texture continuity, or geometry at unvisited locations.

The late cards have not yet been used to establish a flying route. Sea animation, shadows, vehicle exit
and map-edge wrapping remain unverified. The local shape vote alone is not evidence that wrapping
is handled or that it resets safely.

## Limits

The crack at car VBlank 6802 has been traced to an omitted terrain triangle. All corners
of the two bordering polygons are paired and moving; their duplicate shared point gets
the same intermediate position. The intervening triangle from the previous game frame
collapses to zero projected area in the newer frame, so the game omits it. Between those
frames it still covers pixels. Blending reconstructs its corners by the step but rejects
the triangle because it is inside the screen, leaving 69 clear-colour pixels.

A diagnostic continuation of collapsed triangles removes that occurrence but leaves 591
hole pixels in 47 car pictures. Including triangles whose projected orientation changes
reduces this to 253 pixels in 26 pictures, with 94.19% of corners moving and zero stray
corners or resized polygons. Neither experiment is an accepted runtime change. Bypassing
neighbour holds as well increases motion to 99.56% but introduces new defects: 360 car
hole pixels in 31 pictures and a new five-pixel walking occurrence at 1935. A common
terrain placement rule alone therefore does not resolve the visibility problem. These
experiments cover the existing clip windows, not new locations or final acceptance.

- A crack one pixel high opens for a single picture, now and then, along a row where terrain polygons meet across
  the screen while the car drives (52 of 1,001 pictures have hole pixels; 20 meet the default reporting threshold). The polygons on its two sides are not
  a complete mesh between the game frames when an intervening triangle is culled; other
  occurrences still need individual classification.
- Far terrain that blending holds (a neighbour of it did not pair) and far terrain whose shading changes with
  distance still change at the game's rate: a band across the far distance on the car route.
- The minimap (the globe on the first disc, the flat map later) and all 2D change at the game's rate; so does the
  encounter's opening effect.
- Not covered by a route: the sea, flying, and the map's edges. Neither the local vote nor the camera
  check has been exercised across a map edge; correct wrapping or a safe reset is not established.


## Headless live evidence

At native resolution, the live software output equals the offline output on all 531 compared walking
pictures and all 999 car pictures: zero differing pixels. The comparison crops the offline 320 by 240
canvas to the live 320 by 224 display (rows 8 through 231) and aligns the same presentation interval.
These captures were made after the integration merge. This establishes picture equality, not display pacing.

The headless timing capture reports software presentation mean/p99 of 0.989/1.813 ms over 532 walking
VBlanks and 0.929/1.696 ms over 1,000 car VBlanks with Smooth motion on. Total game-thread CPU time,
including emulation and presentation, was 11.67/36.70 ms and 8.99/23.60 ms respectively. These are one
capture under shared-machine load, not an isolated performance comparison or a frame-rate guarantee.

## Visual defects observed

The car's clear-colour cracks reported at or above the tool's 16-pixel threshold occur at VBlanks
6776, 6802, 6820, 6852, 6880, 6888, 6890, 6892, 6894, 7160, 7162, 7170, 7174, 7178, 7202,
7222, 7226, 7244, 7246 and 7254. The aggregate is 821 pixels, including smaller occurrences below
that threshold. Inspection of 6801–6803 confirms a black horizontal crack in the middle picture
that is absent from the adjacent game pictures. Zero stray corners and zero resized world polygons
therefore do not establish that the pictures are correct.

Repeating the diagnostics with a one-pixel reporting threshold also finds walk 2346 (5 pixels), and town
1958 (6), 1964 (7), 1966 (7), 1970 (7), 1974 (7), 2428 (2), 2430 (2), 2432 (3), 2434 (1),
2436 (4), 2438 (10), 2440 (2), 2442 (14), 2444 (5), 2446 (2), 2448 (8), 2450 (9), 2452 (14).
The town occurrences from 2428 onward are inside the Garden. These are automated hole detections;
they are not all individually confirmed visible defects. The car crack is visually confirmed.

Below the 16-pixel threshold, the car also has holes at 6610 (5), 6618 (5), 6628 (14), 6696 (4),
6698 (2), 6700 (3), 6702 (1), 6704 (1), 6706 (1), 6708 (1), 6774 (5), 6788 (14), 6848 (5),
6858 (10), 6882 (10), 7106 (3), 7148 (15), 7154 (3), 7166 (12), 7168 (9), 7180 (3), 7186 (12),
7188 (9), 7192 (7), 7214 (12), 7230 (2), 7252 (1), 7256 (7), 7260 (10), 7262 (10), 7272 (9)
and 7282 (8): 52 affected pictures total, still 821 pixels. The default report lists only 20 of those pictures.

## Suite receipts

The integrated tree's standard Linux suite selected 4,145 tests: 4,143 passed and two were skipped.
A fresh run with `-Wl,--as-needed` and an external `TMPDIR` containing `..` gives the same counts,
with no failures (492.91 seconds). Both runs use the default player and dummy SDL video/audio drivers.
The omitted cases report an empty GTE public-vector parameter set and an unavailable
`VK_LAYER_KHRONOS_validation`, respectively.
The complete owned-game tier passed all 25 tests with no skips (2,120.90 seconds), including native/
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
