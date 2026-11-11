# Frame presentation

The fourth stage of presenting frames between the game's own: drawing them while the game runs. Frame identity
(`docs/frame-identity.md`) says which vertex is which between game frames, frame blending (`docs/frame-blend.md`)
where 3D stands between them and frame layers (`docs/frame-layer.md`) where a field's background stands; until
now only a developer tool drew the result, offline, from recordings. With the setting on, the player draws it.

## The setting

| Where | What |
|---|---|
| Command line | `--smooth-motion` (or `--smooth-motion 1`; `0` turns it off) |
| Config | `smooth-motion=1` |
| Menu | Video, Smooth motion |

Off by default. Off, nothing of this runs: no recording GTE entries are bound (the interpreter's GTE commands and
the projection entries modules call are the plain ones), nothing observes or taps the guest's GPU, and the
renderer mirrors the guest's GPU as before. The menu switches it during play, from the next VBlank on. With
the frame identity developer switches (`--frame-identity` and the others) it stays off.

## How a frame between two game frames is drawn

The host starts the frame identity service with the recording GTE entries bound, taps the guest's GPU
(`gpu_tap`: every GP0 and GP1 word, read and VBlank) and receives every whole GP0 command it executes through the
service (`frame_identity_config_t`'s `command`). `runtime/frame_present.h` keeps a GPU of its own, the
**presentation GPU**, which the renderer mirrors and the display shows in place of the guest's:

- It keeps the presentation GPU's state at the start of the newest game frame (the **checkpoint**) and a journal
  of everything the guest sent since. The guest's commands feed frame blending (each polygon's look) and frame
  layers (each command of the frame) as they come; each identity frame, at the guest's display-start change,
  goes to both.
- To show the game at some moment, it puts the checkpoint back and draws the journal again: the newest frame's
  polygons, drawing offsets and sprites where blending and its layers put them at that moment's fraction of the
  interval, the colours between, what the newer frame culled at the screen's edge beneath it, and everything
  after the frame's display-start change (the HUD, menus, the cursor) as it was, so those stay current. At the end
  of the interval the picture is the guest's, pixel for pixel. The rules are those of the offline renderer; a
  test draws the synthetic scenes of both stages offline and live and finds the same picture at every VBlank.
- After the display-start change the guest draws the next frame into the other buffer. None of that reaches the
  picture and none of it is kept, so drawing commands whose drawing area misses the shown picture are left out;
  uploads, copies and state are drawn. This halves the cost in battle and changes no presented pixel.
- A frame drawn at fraction 1 becomes the next checkpoint on the way. A frame the next one cut short is drawn
  once more, whole and unseen, when the next ends. More than eight VBlanks without a frame (a load) and the
  checkpoint follows the guest, so the journal stays short.

Nothing here writes to the guest: the presentation GPU is a separate `gpu_t`, copied from the guest's when
presentation starts or a state is loaded (`gpu_copy`).

### Software renderer

The checkpoint is the presentation GPU's whole state, VRAM included: one copy of a little over a megabyte
(`gpu_copy`) each way, about 0.1 ms. Measured below.

### Vulkan renderer

The renderer keeps VRAM on the GPU in two planes (native and scaled) and an atlas that says which plane holds
each block. Copying it whole for every picture would cost 32 MB at 4x and 128 MB at 8x of copies each way. The
host instead notes the 64x64-pixel tiles of VRAM written since the checkpoint was kept or put back (draws by
their scissor, fills, copies, uploads); `Renderer::copy_vram` copies only those tiles of both planes (and of the
multisampled plane) and their atlas blocks, between the renderer's images and a second set it keeps. In play
that is the back buffer and a few textures.

The software rasterizer runs for the presentation GPU as for the guest's: the renderer takes some pixels from it
(feedback textures, copies it draws natively), and at 1x it is the picture. Above 1x it is left out of the pictures
between game frames, which nothing is kept from (`host_gpu_between`); when such a picture needs software pixels
after all, VRAM is first taken back from the GPU, and the pictures after it keep the rasterizer on until one
passes without needing them, so a stretch of effects does not wait on the GPU for every picture. Whole pictures,
and therefore every checkpoint, are drawn exactly as before.

Fractions of a pixel: at an internal resolution above 1x, the corners blending moves are drawn where the GTE's
equations put them before rounding (`frame_blend_fractions`), less the share of the game's own rounding at each
end, so both ends are still the game's pixels and the motion between them is even; drawing offsets and sprites
of moving layers likewise (`frame_layer_offset_fraction`, `frame_layer_sprite_fraction`). They reach the
renderer beside the commands (`gpu_subpixel`, `gpu_subpixel_offset`). A layer moves as a whole, every tile by
the same fraction, so tiles keep their shared edges.

Changes inside `third_party/parallel-psx`: `FBAtlas::get_status`/`set_status`, `Renderer::copy_vram` and its
`VramCopy`, and device creation enables `VK_KHR_present_id`, `VK_KHR_present_wait` and
`VK_EXT_present_mode_fifo_latest_ready` where the device has them.

## Pacing

With a window and the Vulkan renderer, pictures come on the display's own clock, not on the guest's VBlanks, and
from a thread of their own: the guest's thread only queues what it sends its GPU and each identity frame, and the
presentation thread takes the queue up, draws and presents (`host_frame_present_queue`, `host_display_presenter`).
The game thread parks it to use the renderer itself (the menu, a loaded state's first picture, shutting down).
The guest's timing and audio are not touched: the guest runs on the audio clock as before, and nothing it does
waits on a picture.

A new picture is presented every k-th refresh of the display, k the refresh rate over 60 rounded (1 at 60 Hz, 2
at 120 or 144 Hz: about 60 pictures a second), each at the same moment of a refresh's period: just before a
refresh, which it is always too late for, so that it is always in time for the next. Where the device can say
when each present reaches the display (`VK_KHR_present_wait`), a thread watches each present arrive
(`HostGPU::Swapchain::watch`) and the moments follow those times, so they do not drift against the display; a
picture that still makes the earlier refresh halves the margin. The swapchain shows the latest picture ready at
each refresh where the device offers it (`VK_EXT_present_mode_fifo_latest_ready`), so that presenting never
blocks; elsewhere it is FIFO. Without present times the clock runs on its own at the display's nominal rate. The
picture is drawn ahead, by about the most drawing has taken lately, and handed over at its moment. The display
the window is on is checked once a second, and the clock starts over when it moves to one of another rate.

Each picture shows the game at its moment on the guest's VBlank clock (smoothed, since the guest's VBlanks come
when pacing wakes, a millisecond early or late). The phase of the newest game frame follows its measured interval,
as offline: the frame that ended n VBlanks after the one before is drawn at (t - end) / n, whole from the end of
its interval until the next frame ends (a hold), and cut short when the next comes early (a skip). Game frames
come every 2 to 6 VBlanks.

Shown as the game shows it: fast-forward, the world map (excluded by frame identity), a frame more than eight
VBlanks after the one before (loads, a disc change, a field change), a camera cut or a change of scene, and a
state just loaded until two frames have ended. The menu overlay and pause present their retained picture as
before. Movies are drawn like any other frame: with nothing that moves between frames, every picture is the game's.
Without a window (headless) and with the software renderer, the picture is drawn once per guest VBlank on the
guest's own timeline, as the offline renderer does.

`--present-stats PATH.csv` writes one line per picture: the moment it was for, when it was drawn and presented,
when it was seen reaching the display, how many refreshes after its moment that was, and its phase. At exit the
player prints how many pictures it presented and missed and the time spent drawing, handing over and waiting.

## Measures

On the machine this was built on (16 threads, an NVIDIA RTX 4070 Laptop GPU, Hyprland, displays at 144 Hz and a
virtual output at 60 Hz), with native code, at 4x unless said otherwise.

**Live against offline.** Over a battle window of 801 VBlanks (the magic route, 2860 to 3660), the player's
presented pictures with the software renderer, and with Vulkan at 1x, are those of `inbetween` from a recording of
the same run, every one. At 4x every whole picture is the picture with the setting off; the pictures between
differ, as they should. A synthetic test draws the scenes of `test_frame_blend` and `test_frame_layer` both ways and
compares every VBlank.

**Cost per picture,** headless, one picture per VBlank (the CPU difference is what the presenter adds; in a window
it runs on its own thread, not the game's):

| | CPU off | CPU on | GPU on |
|---|---:|---:|---:|
| battle, 1x | 3.3 ms | 8.0 ms | 1.0 ms |
| battle, 2x / 4x / 8x | 3.6 to 3.7 ms | 9.2 to 9.3 ms | 1.2 / 2.1 / 2.4 ms |
| field, 1x | 2.2 ms | 3.8 ms | 0.04 ms |
| field, 2x / 4x / 8x | 2.4 ms | 4.3 to 4.4 ms | 0.2 / 0.2 / 0.4 ms |

With the setting off the guest's work is the same, VBlank for VBlank, and frame times are those of the build before
(`frame-bench`, five alternating runs: p50 3.87 against 3.90 ms with the software renderer, 3.28 against 3.28 ms
with Vulkan).

**Pacing.** In a window on the 60 Hz output, 62 seconds of battle: 98.9% of the intervals between pictures on the
display exactly one refresh (16.67 ms), p99 16.67 ms, 1.1% outside 16.7 +- 1 ms; 63 seconds of the Garden's
fields: 97.1% exact, p99 33.3 ms, 2.9% outside, most within one stretch of seven seconds. On the 144 Hz display,
two refreshes apart (13.9 ms) 91% of the time. The guest kept time throughout: the audio queue never ran dry and
pacing never reached its speed limit (with the presenter on the game thread, as at first, it did).

**Delay.** 3D shows the newer game frame at the end of its interval: one game frame later than whole frames would,
33 ms in fields and 67 ms in battle. A picture is drawn for its moment, on average 0.65 VBlank (11 ms) after the
guest's last VBlank, and reaches the display one refresh later (16.6 ms at 60 Hz). What the guest draws over
the 3D (the HUD, menus, the cursor) is not interpolated: it reaches the display about 11 ms later than with the
setting off, the time to the next picture's moment.

## Limits

- The delay: 3D and layers show the newer game frame only at the end of its interval, one game frame after the
  guest drew it: 33 to 50 ms in fields, 67 to 100 ms in battle. The HUD, menus and cursor are not interpolated,
  but reach the display a little later than with the setting off (above).
- The software renderer draws on the game thread and presents on the guest's VBlanks (59.94 Hz), as before.
- On a display above 60 Hz pictures come every k-th refresh (72 a second at 144 Hz); where the compositor's
  deadline wanders by most of a refresh, as here at 144 Hz, about one picture in ten lands a refresh early or late.
- PGXP has no effect with smooth motion: the presentation GPU draws its own positions.
- Everything frame blending and layers leave at the game's rate stays so (`docs/frame-blend.md`,
  `docs/frame-layer.md`).
