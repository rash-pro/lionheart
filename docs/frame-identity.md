# Frame identity

The first stage of presenting frames between the game's own: for every projected 3D vertex, which
vertex it was in the previous game frame, and for every polygon corner the GPU draws, which projection it came
from. Nothing here draws anything or changes what the game computes; it is off unless a developer switch starts
it, and the default path is unchanged.

## How it works

Every projected vertex passes through the GTE's RTPS or RTPT, from interpreted and native code alike. While the
service runs, the host binds recording versions of those two commands in their place: in `lh_api` for modules
(`native_modules_bind_projection`), in `interp_gte_command` for the interpreter, and, in a developer host
configured with `-DFF8_FRAME_IDENTITY_LINKED=ON`, for recompiled code linked into it (that build compiles linked
code against a forwarding entry, one extra call per projection). Unbound, the plain entries run as they always
did. The recording entries run the plain command and then note the vertex's model-space input, the rotation,
translation and screen constants, the result, and the view-space position `(TR * 4096 + RT * V) >> sf`.

A game frame ends where the guest changes the display start (GP1(05h)); the GPU reports those writes, VBlanks
and every polygon packet to an observer (`gpu_observe`), which costs one test per event without one. At the end
of a frame the service:

- groups its projections into **transform runs**, consecutive projections under one setup (rotation,
  translation, OFX, OFY, H, sf and lm): in practice a bone or a rigid part;
- keys each run by its length and exact model-space inputs, and pairs it with the run of the same key in the
  previous frame; vertex k pairs with vertex k. A key present more than once in either frame pairs only with the
  mutually nearest run (by translation and first vertex in view space), at most half as far as the next candidate
  both ways and within 8 pixels on screen; otherwise it is held;
- gives the frame no history (`FRAME_IDENTITY_RESET`) when it is the first, after `frame_identity_reset` (a
  restored state), or when more than 8 VBlanks passed since the previous end;
- links each corner of each polygon drawn during the frame to a projection: in the newest of this frame and the
  previous one that holds a projection at every corner's position (in this frame, only projections made before
  the polygon was drawn). 3D drawn from the ordering table the CPU built before the swap therefore links to the
  previous frame. Where several projections share a corner's position, the corner is linked only if all of them
  are paired and stood within a pixel of each other in their previous frame, or if exactly one transform run holds
  every corner of the polygon and its projections there did; otherwise it is held.

Fail closed: a corner with no projection at its position (2D), a polygon split across frames, a position shared
with an unpaired projection, an ambiguous one, and anything in a frame without history is held and reported with
its reason. The host may also exclude frames (`FRAME_IDENTITY_EXCLUDED`): an excluded frame publishes no pairs and no
links, but keeps what the mechanism matched in `match` for measurement. FF8's world map, whose terrain the CPU moves
by the camera, was excluded until it passed a check of its own; it is paired by a step (`FRAME_IDENTITY_POINTS`,
`docs/frame-world.md`).

## Interface

`runtime/frame_identity.h` is the whole interface. `frame_identity_start(gpu, config)` delivers each frame to
`config.sink` as a `frame_identity_frame_t`, valid only during the call:

| Field | Meaning |
|---|---|
| `number`, `vblank` | game frames since the start; the VBlank count when the display start changed |
| `first_projection`, `first_polygon` | serials of the first records, counted since the start |
| `display_start`, `tag`, `flags` | the start the frame changed away from; the host's classification; reset, excluded, overflow |
| `setups[]` | rotation, translation, OFX, OFY, H, sf and lm, one per change of setup |
| `runs[]` | `first`, `count`, `setup`, `previous` (paired run or held), `repeats` (same key elsewhere), `key` |
| `projections[]` | `input`, `sz`, `sxy`, `view`, `run`, `previous` (index in the previous frame or held), `match` |
| `polygons[]` | `opcode`, `corners`, packet `xy`, per corner `state`, `age` (0 this frame, 1 the previous) and `projection` |

A consumer keeps the previous frame it was given: a corner of age 1 indexes it, and `previous` always does.
Polygon serials count every polygon packet the GPU executes, in its order, from the start; a recorded GPU stream
replayed from the same point lines up with them by counting its own polygon packets. To
interpolate a linked corner, take its projection's view-space position in its frame and that of the projection's
`previous` in the frame before. Corners in polygons with any held corner, and vertices shared with such polygons,
should stay at the current frame; that rule belongs to the presenter (`docs/frame-blend.md`).

## Developer switch

```sh
lionheart ... --frame-identity                      # summary on stdout at exit
lionheart ... --frame-identity-dump frames.bin      # and the whole stream
```

The summary prints, for all frames and per scene (FF8's mode word: field, world, battle, menu, other), frames,
resets, exclusions, projections paired, polygon corners, corners of 3D polygons (polygons with at least one
corner at a projection), linked corners, corners in polygons whose every corner is linked, and the held corners
by reason, and `jumps`: frames whose paired vertices moved, at the median, 32 pixels or more on screen, which
are camera cuts in practice (right as identity, wrong to blend: blending detects cuts itself, `docs/frame-blend.md`). Then
`Frame identity digest`, an FNV-1a over the stream. Native and interpreted runs of one route must print the same
digest.

The stream starts with the eight bytes `LHFRAME1`, then one record per frame, all little-endian 32-bit words
unless noted: `FRAM`; number, vblank, first projection, first polygon (64-bit each); display start, tag, flags;
setup, run, projection and polygon counts; then per setup the five rotation words, three translations, OFX, OFY,
H, sf, lm; per run first, count, setup, previous, match, repeats, key (64-bit); per projection input x|y<<16,
input z|SZ<<16, SXY, the three view coordinates, run, previous, match; per polygon opcode, corner count, and for
four corners the packet word, state|age<<8 and projection. Held indices are `0xffffffff`.

## The oracle

A developer build configured with `-DFF8_FRAME_ORACLE=ON` (never the release player) adds an independent judge,
`runtime/frame_oracle.c`, that runs only with `--interpreter`:

```sh
lionheart ... --interpreter --frame-oracle report.txt
```

Instruction by instruction it follows which RAM halfword each value came from, through registers, memory, the
scratchpad and the GTE's vertex and matrix registers, across LHU, SLL 16, OR and ANDI 0xffff packing, and
through additions with one untracked operand (kept, marked as offset). It also follows each projection's screen
coordinates from the SXY FIFO through MFC2, SWC2, registers and memory into the words the GPU takes as polygon
corners, from DMA linked lists or block transfers (words the CPU writes to the GP0 port stay undecided). Every
source is checked against the live value before use. The service never reads any of it.

A pair is **correct** when the three input coordinates of both projections came from the same halfwords with the
same write generation, **wrong** when a coordinate came from another halfword and both halfwords were alive in
both frames, **equivalent** when the oracle's own partner stood exactly where the service's did (one point held
in two records), **partial** when some coordinates agree and none contradict, and undecided otherwise. A corner's
link is **correct** when the packet word came from the linked projection, **equivalent** when it came from
another projection at the same position whose pair stood within a pixel of the linked one's, **wrong** otherwise,
and undecided when the oracle cannot tell where the word came from. Identical instances may share one vertex
table, where vertex sources cannot tell them apart; for correct pairs of repeated keys the oracle also compares
where each projection's translation (or, unknown that, rotation) was copied from, and counts `instance-same` and
`instance-other`.

The report has one `F` line per frame (number, vblank, tag, flags, projections, matched, correct, partial, wrong,
equivalent, undecided, corners, linked, link correct, link equivalent, link wrong, link undecided, and held
corners the oracle knows to be 3D), `W` lines describing the first wrong pairs and links of each scene, and `T`
totals per scene. `LOST` means the oracle lost track (native code ran, or its counts disagree with the
service's), and the run fails.

## Routes and checks

`tests/native/cases/test_frame_identity.cpp` covers the mechanism without game data. With the owner's discs and
cards, `test_frame_identity_routes` runs each route with the default player, service off and on, and with the
oracle host (`build/frame-oracle/lionheart`, or `LIONHEART_FRAME_ORACLE`), and holds battle and field 3D to the
bars: at least 90% of the corners of drawn 3D polygons linked, wrong pairs at most 0.01% of the pairs the oracle
decides, wrong links at most 0.1% of the links it decides; the oracle must decide at least 30% of the pairs, the
final state must not change, and both builds must print one digest. Routes that cross the world map add its own
check (`docs/frame-world.md`).

| Route | Card | What it exercises |
|---|---|---|
| `battle-magic.txt` | disc1-early | world map, battle, magic effects |
| `battle-summon.txt` | disc1-early | world map, battle, a summon |
| `play-disc2-prison.txt` | disc2-start | menus, a walk across the prison field, a battle against a robot and two soldiers, Diablos |
| `fps60-identical-enemies.txt` | disc2-start | the same battle with no commands, two identical soldiers acting to the end |
| `fps60-camera-cuts.txt` | disc1-early | an attack every 80 VBlanks, each cutting the camera to the attacker and back |

`test_native_modules_give_the_interpreters_stream` compares the digest of a developer host running modules
(`docs/native-modules.md`) with the interpreted player's.

