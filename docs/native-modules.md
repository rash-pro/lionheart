# Load recompiled code as modules

A player gets native code by compiling it on its own machine and loading it while the game runs, one module per
overlay set plus one for the main executable. Until a module arrives, the interpreter runs that code. This page is
the developer path to exercise that mechanism without the player's own build orchestration.

```sh
build/generator/lionheart-gen --disc disc1.cue --disc disc2.cue --disc disc3.cue --disc disc4.cue \
  --legacy-overlays --shape modules -o build/generated-modules
cmake -S . -B build/dev-modules -DFF8_PLAYER=OFF -DFF8_STATS=ON -DGENERATED_DIR=$PWD/build/generated-modules
cmake --build build/dev-modules --target ff8 -j6
cmake -S tools/native-modules -B build/modules -DGENERATED_DIR=$PWD/build/generated-modules -DFF8_STATS=ON
cmake --build build/modules -j6
build/dev-modules/ff8 build/generated-modules/extracted/SLUS_008.92 --disc ... \
  --native-modules build/modules/modules
```

The host built from a `--shape modules` tree links no game code. `--native-modules DIR` opens every module in
`DIR` before the first instruction; add `--native-modules-pace N:V` to start with none and hand over `N` modules
every `V` VBlanks instead, as a first run would. `FF8_STATS` must match between the host and the modules, as must
the runtime headers: anything else is refused, as is a module generated from another executable. The modules
builder takes `LH_ONLY=main;set-2` to build a subset.

## The contract

`runtime/native_module.h` is the whole binary contract. Each module exports one symbol, `lh_module_descriptor`:
its set index (`-1` for the executable), the base, size and SHA-1 of the image it was generated from, the
identity of the executable that image belongs to, its entry and region tables, and the main-owned functions it
imports. It reaches the runtime only through `lh_api`, a table of the 38 runtime functions generated code can call
(every GTE command the generator can name, used or not), which the host fills while binding; everything else a body needs is inlined from `cpu.h`. Modules link nothing,
so a module that needs any other symbol fails to link instead of failing to load.

The ABI identity is a SHA-256 of the headers module code is compiled against and of the settings that shape them
(`cmake/native_module_abi.cmake`); both sides compile it in. The descriptor also records the layout the compiler
actually produced, so a module built by another compiler with the same headers loads only if it agrees.

The host (`backends/host/native_modules.c`) checks the file's extent before mapping it, then the identity, the
tables, and for the main module every function's code against the executable it booted. Overlay code is checked
when it runs, by the same fingerprints the linked build uses. A module is registered only at the top of the
dispatch loop, where nothing holds a registry pointer; registered modules stay loaded until shutdown, and must:
a function that several sets contain is compiled into each of their modules, and the host points every set's
entries at the copy registered first, as the linked build shares one body between sets.
