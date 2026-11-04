# Lionheart architecture

Lionheart statically recompiles the original PlayStation executable and overlays into C99.
The portable runtime provides the guest CPU, memory and hardware model.
The SDL2 host supplies input, audio, storage and presentation.
The distributable player uses that runtime's interpreter and an empty catalog.
No game-derived code or data is committed or distributed.

## Disc to native executable

`tools/psxdisc.py` reads raw 2352-byte CUE/BIN images and their ISO9660 directories.
Setup extracts the boot EXE and verifies its size and SHA-1 against `games/ff8-usa`.
`recomp/disc.py` extracts and verifies the executable overlays using addresses, sizes and hashes.
The setup uses the versioned top-table catalog, including movie overlays, across the supplied discs.
`recomp/overlays.py` also supports the wider loader/effect catalog for diagnostics and legal scanning.
All extracted bytes stay in the build directory.

`recomp/discover.py` follows function entries and control flow. `decode.py` decodes MIPS instructions.
`emit.py`, `fast.py` and `precise.py` generate C with explicit guest registers, loads and retirement boundaries.
Generated translation units and dispatch tables are compiled together with `runtime/` and `backends/host/`.
Version metadata contains identities, addresses, boundaries and resource locations, never payload bytes.

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
State loading preserves live cards by default. The optional localhost state bridge and local achievements
are inactive unless configured; they contact no external service.

## Presentation

SDL2 handles controllers, keyboard, audio and windows. The software renderer presents the native framebuffer.
Vulkan uses the vendored parallel-psx renderer for scaled output, with CRT and screenshot composition.
PGXP tracks precise transformed coordinates for presentation while preserving guest registers and VRAM.
Native soft PNGs and complete guest dumps remain the exactness reference.

The ImGui shell selects discs and cards before starting the guest. Esc or Guide opens the in-game menu.
Its modal loop retains the picture, pauses audio and guest execution, then clears held input before resuming.
Settings use an atomic INI writer that preserves comments and unknown keys.
