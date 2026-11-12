# Frame presentation

The fourth stage of presenting frames between the game's own: drawing them while the game runs. Frame identity
(`docs/frame-identity.md`) says which vertex is which between game frames, frame blending (`docs/frame-blend.md`)
where 3D stands between them and frame layers (`docs/frame-layer.md`) where a field's background stands; until
now only a developer tool drew the result, offline, from recordings. With the setting on, the player draws it.

The world map is enabled by the fifth stage; its independent check and remaining visual defects are
documented in `docs/frame-world.md`.

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
  once more, whole and unseen, when the next ends.
- More than eight VBlanks without a frame (a load, a movie), or none yet since the presenter started (the
  screens a game starts with), and the checkpoint follows the guest, so the journal stays short. The picture is
  then the guest's as of its last VBlank: a guest that ends no frames draws into the picture it shows, and what
  it has drawn of the next one when the display asks is not a picture the game shows.

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
(feedback textures, copies it draws natively), and at 1x it is the picture. Above 1x, in the pictures between game
frames, which nothing is kept from (`host_gpu_between`), it draws only the primitives that meet the parts of VRAM
(in blocks of 16x16 pixels) the picture before read software pixels from: the sources of copies the renderer
draws natively, and the textures and palettes of the primitives it drew in software. The pictures of a game frame
draw the same commands, so these are the parts this picture reads too; battle captures the picture it shows every
frame, and of some 2,000 triangles a picture about 20 meet them. Where a picture reads a block it left out after
all, VRAM is first taken back from the GPU. Whole pictures, and therefore every checkpoint, are drawn exactly as
before.

Each textured primitive asks the renderer's atlas whether the texture it reads was drawn to since, scanning every
block of the texture page; drawing the same commands again, nearly all of these ask the same of an unchanged
atlas. With smooth motion the atlas skips the scan of a texture read again with no block's status and no render
pass changed since (`Renderer::set_texture_memo`).

Fractions of a pixel: at an internal resolution above 1x, the corners blending moves are drawn where the GTE's
equations put them before rounding (`frame_blend_fractions`), less the share of the game's own rounding at each
end, so both ends are still the game's pixels and the motion between them is even; drawing offsets and sprites
of moving layers likewise (`frame_layer_offset_fraction`, `frame_layer_sprite_fraction`). They reach the
renderer beside the commands (`gpu_subpixel`, `gpu_subpixel_offset`). A layer moves as a whole, every tile by
the same fraction, so tiles keep their shared edges.

Changes inside `third_party/parallel-psx`: `FBAtlas::get_status`/`set_status`, `Renderer::copy_vram` and its
`VramCopy`, the atlas' texture memo (`Renderer::set_texture_memo`), and device creation enables
`VK_KHR_present_id`, `VK_KHR_present_wait`, `VK_EXT_present_mode_fifo_latest_ready`, `VK_EXT_present_timing` and
`VK_KHR_present_id2` where the device has them.

## Pacing

With a window and the Vulkan renderer, pictures come on the display's own clock, not on the guest's VBlanks, and
from a thread of their own: the guest's thread only queues what it sends its GPU and each identity frame, and the
presentation thread takes the queue up, draws and presents (`host_frame_present_queue`, `host_display_presenter`).
The game thread parks it to use the renderer itself (the menu, a loaded state's first picture, shutting down).
The guest's timing and audio are not touched: the guest runs on the audio clock as before, and nothing it does
waits on a picture.

A picture can be drawn for any moment, so each is drawn for one refresh of the display and shown on it: on every
refresh where a picture's cost fits the refresh period with a margin, on every second (or third...) where it does
not. The pacing is `backends/host/present_pace.h`, apart from the device so that a test can drive it on synthetic
displays.

**When a picture was shown.** Where the device has `VK_EXT_present_timing` (and the window's surface supports it),
each present carries an id and asks when its first pixel went out; the presentation engine reports the
compositor's own times, read back on the presentation thread (`HostGPU::Swapchain::shown`), in a swapchain time
domain calibrated against the host's monotonic clock. `VK_KHR_present_wait` cannot stand in for this on every
driver: on this machine's NVIDIA driver a wait only completes when some call into the swapchain takes up the
compositor's events, so a watcher sees each present when the next present is handed over, and pacing by those
times locks onto its own hand-overs instead of the display. That was the seven seconds of misses in the fields at
60 Hz: the moments followed themselves, and handing over a picture took from 1 to 10 ms (inside the driver's
present path), so some crossed the compositor's deadline and some did not. Without present timing the old
watcher is still used.

**The grid.** The refreshes are a grid: its period is the one the device reports (`refreshDuration`), or, where
it reports none (a virtual output; this machine's 60 Hz output runs at 59.90 Hz), found from the time between
reports on the grid. Its phase is locked onto the times most reports agree on and follows each report on the grid
after that, so it does not drift against the display. A compositor may report a picture at another output's
refresh (Hyprland does for a few percent of pictures here, at the laptop panel's refresh while the window is on
the other display): a report off the grid by more than a tenth of a refresh says nothing of the picture and is set
aside; many in a row and the grid is found again.

**Handing over.** With present timing and presents that can be held until a time (`presentAtAbsoluteTime`), each
picture is handed over before the refresh before its own and held until half a refresh before its own. A
compositor takes what is due at each refresh (Hyprland releases a held commit at the refresh before the one it is
for, or by a timer), so the picture is the compositor's a refresh ahead and shown on its own refresh wherever in
the period the compositor's deadline lies, and wherever the grid is known to within half a refresh. The swapchain
is a mailbox then, with room for the pictures held: with FIFO each present also waits for the one before to be
shown, which Hyprland only does when something else on the output changes, and pictures stalled for seconds. A
picture that comes late moves the hand-over a millisecond earlier, twice as much as the last time when pictures
come late within ten seconds of each other (the deadline moved), up to half a refresh; it comes back by half a
millisecond every five seconds with none late. Between two pictures every second refresh or more, copies of the picture are handed
over for each refresh between, held until theirs: a compositor that has had nothing new for two refreshes takes
one more to wake up, which made every third or fourth picture late. Without targets, pictures are handed over in
the middle of the refresh before the one before their own and moved within it as they come early or late.

**How often.** What a picture costs is about the most the last sixteen took (the second most, so that one slow
picture does not move it), copies by their average. A picture every m refreshes, m the least for which a picture
and m - 1 copies take three quarters of m refreshes; up as soon as the current multiple no longer fits with a
little to spare (a picture that does not fit makes the next late), down only after two seconds in which the
lower one would have fitted; never fewer than about 45 pictures a second (every refresh at 60 Hz, at most every
third at 144 Hz), since at 60 Hz every second refresh would halve the motion for seconds where a costly stretch
only makes a few pictures late. A change of multiple changes nothing in how the game moves, since every picture is
drawn for its own moment: the steps between pictures double or halve, evenly.

**What a picture shows.** A picture shows the game at the moment it is begun, held at the same distance from the
refresh it is for, so that pictures evenly spaced on the display show the game evenly spaced. That moment is
taken on the guest's VBlank clock (smoothed, since the guest's VBlanks come when pacing wakes, a millisecond
early or late). The phase of the newest game frame follows its measured interval, as offline: the frame that
ended n VBlanks after the one before is drawn at (t - end) / n, whole from the end of its interval until the next
frame ends (a hold), and cut short when the next comes early (a skip). Game frames come every 2 to 6 VBlanks.

The display the window is on is checked once a second, and pacing starts over when it has moved to one of
another rate (seen on two checks in a row: a window can be said to be on another display for a moment) or the
device learns more of it.

Shown as the game shows it: fast-forward, a frame more than eight
VBlanks after the one before (loads, a disc change, a field change), a camera cut or a change of scene, and a
state just loaded until two frames have ended. The menu overlay and pause present their retained picture as
before. Movies are drawn like any other frame: with nothing that moves between frames, every picture is the game's.
Without a window (headless) and with the software renderer, the picture is drawn once per guest VBlank on the
guest's own timeline, as the offline renderer does.

`--present-stats PATH.csv` writes one line per present: the refresh it was for, when it was begun and handed
over, when the display reported it (and whether that report was off the grid), how many refreshes after its own
that was, its phase, the refresh period, the multiple, the time it was held until, whether it is a copy, and when
the guest VBlank whose game it shows came. At exit the player prints how many pictures were on time, late or
reported off the grid, the multiple and the lead, and the time spent drawing and handing over.

## Measures

On the machine this was built on (16 threads, an NVIDIA RTX 4070 Laptop GPU, Hyprland, displays at 144 Hz and a
virtual output at 60 Hz), with native code, at 4x unless said otherwise.

**Live against offline.** Over a battle window of 801 VBlanks (the magic route, 2860 to 3660), the player's
presented pictures with the software renderer, and with Vulkan at 1x, are those of `inbetween` from a recording of
the same run, every one; so are 601 VBlanks of the Garden's corridor (2940 to 3540). At 4x every whole picture is
the picture with the setting off; the pictures between differ, as they should. A synthetic test draws the scenes
of `test_frame_blend` and `test_frame_layer` both ways and compares every VBlank.

**Cost per picture,** headless, one picture per VBlank, the game thread's CPU (the difference is what the presenter
adds; in a window it runs on its own thread, not the game's), GPU per picture with the setting on:

| | CPU off | CPU on | GPU on |
|---|---:|---:|---:|
| battle, 1x | 3.4 ms | 8.1 ms | 1.0 ms |
| battle, 4x | 3.7 ms | 6.0 ms | 1.0 ms |
| field, 1x | 2.2 ms | 3.8 ms | 0.04 ms |
| field, 4x | 2.4 ms | 3.5 ms | 0.2 ms |

Before drawing less again (the software rasterizer only where its pixels are read, the atlas' texture memo), battle
at 4x took 9.3 ms (GPU 2.2 ms) and fields 4.3 ms; at 1x the software rasterizer is the picture and the cost is that
of drawing it. In a window, a battle picture at 4x takes 2.5 ms to draw and hand over at the median, 7 to 10 ms at
the 95th percentile (heavy effects), and a copy for a refresh between about 1.2 ms.

With the setting off the guest's work is the same, VBlank for VBlank, and frame times are those of the build before
(`frame-bench`, five alternating runs: p50 3.87 against 3.87 ms with the software renderer, 3.28 against 3.27 ms
with Vulkan).

**Pacing,** in a window, from the presentation engine's reports; the intervals are between the refreshes pictures
were shown on (a report off the grid is taken as on time: a few percent of reports here are another output's
refresh).

| | pictures on the refresh drawn for | pictures a second | intervals |
|---|---:|---:|---|
| 60 Hz output, battle, 59 s | 100% | 59.7 | 99.37% one refresh, p99 16.70 ms, 0.63% outside 16.7 +- 1 ms |
| 60 Hz output, Garden's fields, 61 s | 99.97% | 59.8 | 99.53% one refresh, p99 16.70 ms, 0.47% outside 16.7 +- 1 ms |
| 144 Hz display, battle, 59 s | 99.98% | 78.4 | every second refresh, every refresh in light stretches, every third in the heaviest effects; 99.8% equal to the multiple |
| 144 Hz display, Garden's fields, 61 s | 99.87% | 98.3 | every refresh, every second in the hall and lobby; 99.8% equal to the multiple |

The 60 Hz output is virtual: it runs at 59.90 Hz, which its reports give and the device does not. Full screen on
the 144 Hz display, 14 seconds each: fields 99.83% (128 pictures a second), battle 100% (72 a second). The audio
queue never ran dry. In the battle's heaviest effects the guest runs at its pacing's speed limit for 1.3 to 1.7% of
its VBlanks, as the same window does with the setting off (4.2%).

**Delay.** 3D shows the newer game frame at the end of its interval: one game frame later than whole frames would,
33 ms in fields and 67 ms in battle. What the guest draws over the 3D (the HUD, menus, the cursor) is not
interpolated: measured from the guest's VBlank to the first picture that shows it on the display, it takes 32 to
35 ms at 60 Hz and 22 to 26 ms at 144 Hz, against about 17 and 8 ms with the setting off (a present reaches the
display 15.7 ms and 7.4 ms after it is handed over, on average). The difference is the compositor's: with the
setting off, a picture now and then reaches a compositor with nothing pending, which shows it at its next refresh;
with the setting on there is a picture for every refresh, and the compositor takes each only before the refresh
before its own, a refresh more. Then the picture's drawing, budgeted at about the most a picture took lately (a
late picture costs a stutter), and the wait for the next picture to begin, half a picture's interval on average
where the guest's own presents do not wait. Drawing the HUD later than the picture under it would shorten it, at
the cost of the two showing different moments.

## Limits

- The delay: 3D and layers show the newer game frame only at the end of its interval, one game frame after the
  guest drew it: 33 to 50 ms in fields, 67 to 100 ms in battle. The HUD, menus and cursor are not interpolated,
  and reach the display about a refresh and a picture's drawing later than with the setting off (above).
- The software renderer draws on the game thread and presents on the guest's VBlanks (59.94 Hz), as before.
- Without `VK_EXT_present_timing` (other drivers, Windows) the presenter paces by `VK_KHR_present_wait`, whose
  times may be those of its own hand-overs, as here before.
- A compositor may report a picture at another output's refresh; such reports are set aside, and say nothing of
  whether that picture was on time.
- Variable refresh is not used: with it a display shows each picture when it is handed over, and the grid the
  pacer locks onto would follow its own presents.
- PGXP has no effect with smooth motion: the presentation GPU draws its own positions.
- Everything frame blending and layers leave at the game's rate stays so (`docs/frame-blend.md`,
  `docs/frame-layer.md`).
