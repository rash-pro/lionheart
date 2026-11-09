# Lionheart architecture

Lionheart statically recompiles the original PlayStation executable and overlays into C99.
The portable runtime provides the guest CPU, memory and hardware model.
The SDL2 host supplies input, audio, storage and presentation.
The distributable player uses that runtime's interpreter and an empty catalog.
No game-derived code or data is committed or distributed.

## Disc to native executable

`generator/` is a standalone C99 library and the `lionheart-gen` command-line tool. Neither depends on
Python, SDL or the runtime. The library owns each call's state, reports progress and cancellation through
callbacks, and unwinds failures back to the caller. Paths are UTF-8 and output uses binary writes with LF on
Linux and Windows.

`generator/src/disc.c` reads raw 2352-byte CUE/BIN images and ISO9660 directories. `catalog.c` verifies the
EXE and overlays against `games/ff8-usa`, and `lzs.c` decompresses stored resources. The developer setup uses
the versioned top-table catalog, including movie overlays, across the supplied discs. The default CLI mode
also supports the wider loader/effect catalog for diagnostics and legal scanning. All extracted bytes stay
in the build directory. Metadata contains identities, addresses, boundaries and locations, never payload bytes.

`decode.c` decodes MIPS instructions and `discover.c` follows function entries and control flow. `emit.c`,
`fast.c` and `precise.c` generate C with explicit guest registers, loads and retirement boundaries.
`locals.c` handles selected register caching for experimental timing. Generated translation units and dispatch
tables compile together with `runtime/` and `backends/host/`.

`tools/setup.cmake` builds the small host generator, invokes the library's cached developer generation, then
configures and builds the developer executable. Cache identity lives in `generator/src/cache.c` for use by
embedding hosts. CMake fingerprints the generator's C sources and headers with checkout-relative names.
The key also covers catalog bytes, options, the verified EXE and overlay identities, and the disc set.
Every hit verifies the supplied inputs and each generated output's SHA-256. The receipt is removed before
emission and published by an atomic rename after success. Callers must exclusively own the output directory;
the setup launcher locks `build/` for its whole run. Caches from the old setup are not accepted.

The restoration ABI hashes that same native generator fingerprint, the generated manifest, runtime and host
sources, and the statistics and interpreter options. Replacing the Python source hashes changes the build
identity and rejects older save states. Memory card saves do not depend on this identity.

## Dispatch and overlays

`cpu_t` owns registers, RAM, scratchpad and dispatch state. Memory helpers distinguish RAM mirrors,
scratchpad and hardware registers. Compiled code observes load delay slots, branch delay slots and code writes.
Direct calls and returns use native C where safe; indirect transfers resolve through address dispatch.
Missing or invalidated regions fall back to the interpreter. Immutable code spans identify native regions.

FF8 replaces overlays in RAM during play. Dispatch tracks their load addresses and content identities,
invalidates changed code and preserves the active module across nested calls and callbacks.
The interpreter also covers code not discovered during translation.

## Timing and hardware

The guest clock advances at instruction retirement. Pending events can shorten a compiled execution deadline.
Interrupt boundaries, DMA, timers, CD-ROM, MDEC and SPU work share the same event scheduler.
Exact timing is the default. Experimental compact/block timing is intended for local investigation.
Register-level hardware behavior remains separate from host presentation.

The software GPU owns native VRAM and produces deterministic native frames.
Memory cards are files. Save states include runtime hardware state and require the same build identity.
State loading preserves live cards by default. The optional localhost state bridge and local evaluator
contact no external service. The separate RetroAchievements client is opt-in and casual only: a worker handles
HTTP, disc hashing and badge files; the host delivers responses to rc_client between frames or during the menu
pause. Memory reads access only guest RAM and scratchpad. Authentication, badges and unlocks never enter
the guest state. While a menu is open, rc_client_idle services requests without evaluating achievements.

## Presentation

SDL2 handles controllers, keyboard, audio and windows. The software renderer presents the native framebuffer.
Vulkan uses the vendored parallel-psx renderer for scaled output, with CRT and screenshot composition.
PGXP tracks precise transformed coordinates for presentation while preserving guest registers and VRAM.
Native soft PNGs and complete guest dumps remain the exactness reference.

The ImGui shell selects discs and cards before starting the guest. Esc or Guide opens the in-game menu.
Its modal loop retains the picture, pauses audio and guest execution, then clears held input before resuming.
Settings use an atomic INI writer that preserves comments and unknown keys.
