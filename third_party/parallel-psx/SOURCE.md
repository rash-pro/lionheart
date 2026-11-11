# parallel-psx host port

Origin: https://github.com/libretro/beetle-psx-libretro
Commit: e150d6229478044875d5924e8921cc4685ab9f94, ONLY `parallel-psx/`.
Vendored from the upstream directory at the commit above.

Directory licence: ISC (`COPYING`); Vulkan/util headers: MIT (notices retained).
volk: MIT notice in volk.h. SPIRV-Cross and bundled Khronos headers are Apache-2.0; both are omitted here. The host uses installed Vulkan
headers and a small, original SPIR-V layout reader instead. No stb source is shipped. No code outside this directory is used.

Local changes: stderr logging replaces libretro logging; a disabled texture
tracker adapter removes all libretro/thread/filesystem dependencies. Unused custom-texture reference sources are omitted.
Further local changes: trailing whitespace normalized; current Vulkan headers use MAX_ENUM sentinels;
raw scanout uses RGBA8 and nearest sampling;
zero-count attachment copies avoid null pointers under UBSan; the debug-only
libretro texture log is removed; the disabled tracker gets a valid
dummy texture binding; direct-colour textures also apply the texture window.
Prebuilt shader includes were regenerated offline with the installed glslc from
the included sources, using glsl/rebuild_shaders.sh. The SPIR-V layout reader
is original host-port code, not derived from SPIRV-Cross.

Host adapter and build file are new. Later upstream GPL implementations and
DuckStation code have not been consulted or copied.

Additional host changes: an original integer-coordinate scanout shader
crops/quantizes/wraps the visible image on the GPU, including packed 24-bit scanout.
Frame submission can defer queue draining to combine rendering and presentation;
external signal semaphores are owned by each swapchain image. Three frame contexts
poll fences before recycling; only a saturated ring applies backpressure.
Timestamp queries are retrieved without WAIT after the owning fence completes;
all query pools are visited correctly. Submission/backpressure counters are added.

Additional renderer changes: present-compatible device ranking with the existing
index override; optional swapchain-maintenance1 present fences; nonthrowing query
error reporting and checked fence results; exact dual-plane partial uploads that
preserve scaled neighbors within atlas blocks; shader row-parity clipping;
floor-based direct-color texture windows; visible-region packed 24-bit scanout.
All associated prebuilt shader includes were regenerated from these sources.

## Host movie uploads

`glsl/host_movie.comp`, its prebuilt variants, `Renderer::host_movie_upload`,
and the HD branch in `host_scanout.frag` are original project code under MIT.
They update the scaled presentation plane in guest upload order. Bit 16 in the
GPU-private native texture marks an RGB24 replacement. The low 16 bits and the
software raster remain unchanged. No custom-texture replacement code is enabled.
The atlas marks RGB15 movie uploads as rendered textures so guest textured
background draws sample the scaled plane. The original SPIR-V reader now
recognizes SPIR-V 1.0 BufferBlock storage buffers. Run `glsl/rebuild_shaders.sh`
from its directory to regenerate the includes.
Local change: vertex colors use noperspective interpolation. Only UVs use
the supplied clip W, preserving affine Gouraud color with perspective textures.
The affected primitive and feedback shader includes were rebuilt offline.

## framebuffer effects

The scaled and scaled-MSAA VRAM blit shaders now apply `mask_or` to alpha,
including masked copies. This preserves PS1 bit 15 when a copied framebuffer
becomes a semitransparent texture. The four corresponding prebuilt SPIR-V
includes were rebuilt offline with the installed `glslc`.

The host adapter tracks native 15-bit framebuffer feedback through copies and
textured draws. That logic is outside this vendored directory. These patches
are original project changes under MIT. No additional upstream, GPL, or
DuckStation source was used.

## pipeline warmup and timings

`Renderer::warmup` warms primitive states, both texture domains, VRAM copies,
resolves, field modes, and scanout before guest execution. The host then
replaces both planes with its native VRAM snapshot. Gameplay retains the
existing asynchronous submission path. Optional counters measure wall time and
count for actual Vulkan pipeline creation calls.

These patches are original project changes under MIT.

## Portable color conversion

The primitive and programmable-feedback fragment shaders explicitly truncate
8-bit color codes before writing UNORM attachments. The former half-code bias
assumed round-to-nearest conversion, which Vulkan does not require. The existing
0.01-code integer-boundary margin is retained; no pixel-test tolerance changes.
The eleven affected fragment shader includes were rebuilt with local `glslc`.
The three SABR filter widths have distinct identifiers again, preserving the
45/30/60-degree constants and uses encoded in the original prebuilt SPIR-V.

## Atlas dependencies

The atlas retains both accesses of a queued VRAM copy and both writes of a
partial host upload after any intervening barrier. Global stage barriers also
make retired writes visible to color attachment loads for either VRAM plane.
These are original project changes under MIT.

## Smooth motion

The renderer can keep a copy of its VRAM, both planes and the atlas' block status, and put it back, tile by tile
(`Renderer::copy_vram`, `FBAtlas::get_status`, `FBAtlas::set_status`), for a host that draws the same commands
again from one point. Device creation enables `VK_KHR_present_id`, `VK_KHR_present_wait` and
`VK_EXT_present_mode_fifo_latest_ready` where the device has them, for a host that paces its own presents; a
feature chain started by swapchain-maintenance1 now continues after it. These are original project changes
under MIT.
