# Frame identity

The first stage of presenting frames between the game's own (issue #4): for every projected 3D vertex, which
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
  mutually nearest run in view space, at most half as far as the next candidate both ways and within 8 pixels on
  screen; otherwise it is held;
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
its reason. The host may also exclude frames: the developer host and the player exclude FF8's world map
(`FRAME_IDENTITY_EXCLUDED`), which pairs too poorly to be trusted until it has a check of its own. An excluded
frame publishes no pairs and no links, but keeps what the mechanism matched in `match` for measurement.

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

A consumer keeps the previous frame it was given: a corner of age 1 indexes it, and `previous` always does. To
interpolate a linked corner, take its projection's view-space position in its frame and that of the projection's
`previous` in the frame before. Corners in polygons with any held corner, and vertices shared with such polygons,
should stay at the current frame; that rule belongs to the presenter.

## Developer switch

```sh
lionheart ... --frame-identity                      # summary on stdout at exit
lionheart ... --frame-identity-dump frames.bin      # and the whole stream
```

The summary prints, for all frames and per scene (FF8's mode word: field, world, battle, menu, other), frames,
resets, exclusions, projections paired, polygon corners, corners of 3D polygons (polygons with at least one
corner at a projection), linked corners, corners in polygons whose every corner is linked, and the held corners
by reason; then `Frame identity digest`, an FNV-1a over the stream. Native and interpreted runs of one route must
print the same digest.

The stream starts with the eight bytes `LHFRAME1`, then one record per frame, all little-endian 32-bit words
unless noted: `FRAM`; number, vblank, first projection, first polygon (64-bit each); display start, tag, flags;
setup, run, projection and polygon counts; then per setup the five rotation words, three translations, OFX, OFY,
H, sf, lm; per run first, count, setup, previous, match, repeats, key (64-bit); per projection input x|y<<16,
input z|SZ<<16, SXY, the three view coordinates, run, previous, match; per polygon opcode, corner count, and for
four corners the packet word, state|age<<8 and projection. Held indices are `0xffffffff`.
