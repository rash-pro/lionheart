# Load recompiled code as modules

A player gets native code by compiling it on its own machine and loading it while the game runs, one module per
overlay set plus one for the main executable. Until a module arrives, the interpreter runs that code. The first part
of this page is the developer path to exercise that mechanism by hand; [the player's own build](#the-players-own-build)
describes what the release player does.

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

## The player's own build

With a compiler, the release player starts a build service (`backends/host/native_build.h`) right after it boots
the executable. The game starts on the interpreter as before; each module is registered at a dispatch boundary as
it arrives, and the first registration switches the guest off interpreter-only mode. Without a compiler nothing
starts and the player behaves as it always did.

### Which compiler

`native_setup_configure` (`backends/host/native_setup.h`) chooses once per launch, in this order:

1. Zig installed by the player under `<data>/toolchain/<version>/`, with a receipt that matches the pinned archive
   (`data/toolchain/README.md`). Once a player has it, a system compiler installed later changes nothing.
2. On Linux, `clang`, then `gcc`, from absolute folders on the `PATH`. Clang comes first because it builds the
   game's code faster than GCC and is the compiler inside Zig. Neither name is trusted: the player asks the program
   what it is by preprocessing a file that names `__clang_version__` or `__VERSION__` (`native_build_identify`),
   which picks the family and its recipe, then builds a small module from source text with that recipe and the
   same headers, support units and options a real module gets (`native_build_try`). The loader checks it as it
   checks a module, apart from the executable's identity, which no module of its own has (`native_module_try`:
   the file's extent, the descriptor's version, size, ABI id and layout traits, its tables and its call table),
   and its code runs once on a blank processor and must compute the expected value. A compiler that fails any of
   this is passed over, with a line on stderr, as if it were not there.
3. Otherwise none: the third setup step and the hub's native code row offer the download, or an archive downloaded
   by hand, as the player always did. Windows has only this.

The check runs in `<data>/native/check/`, one player at a time (`check.lock`), and the folder is removed again.
Its verdict is kept in `<data>/native/compiler`, keyed by the compiler file's path, device, inode, size and
modification time, the module ABI id and the recipe, so later launches start no compiler before building; any
change to these tries the compiler again. Only accepted compilers are recorded. Removing that compiler makes the
next launch find another, or none, which then asks for the download like a player that never had one. Setup shows
its compiler step only when no compiler was there at launch; with one it ends at the memory card, and the native
code row names it (`GCC 16.2.1`, `Clang 23.1.3`) until the build has sets to count.

### Recipes

Every family compiles each unit with `-O2 -s -fomit-frame-pointer -std=c99 -DNDEBUG -fPIC -w`, no caret or colour
in diagnostics, `-DFF8_NATIVE_MODULE` and the ABI id, and links `-shared -nostdlib -s` with
`-Wl,--no-undefined` (not on Windows): modules link nothing and export only their descriptor, since everything
else in module code is hidden by `native_module.h`.

| Family | Code generation | Diagnostics | Driver |
| --- | --- | --- | --- |
| Zig | `-mcpu=baseline`, `-target x86_64-linux-gnu.2.17` or `x86_64-windows-gnu` (`-mno-stack-arg-probe` there) | `-fno-caret-diagnostics -fno-color-diagnostics` | `zig cc`; its caches go to the build folder |
| Clang | `-march=x86-64 -mtune=generic -fno-stack-protector -U_FORTIFY_SOURCE` | `-fno-caret-diagnostics -fno-color-diagnostics` | `TMPDIR` is the build folder |
| GCC | as Clang | `-fno-diagnostics-show-caret -fdiagnostics-color=never` | as Clang |

`-march=x86-64` is the processor Zig's `-mcpu=baseline` names. Distributions build their compilers to add a stack
canary and checked string functions by default, both of which call into a C library that modules do not link; the
two options turn them off. `native_build_recipe(family)` names each recipe, and the name is part of the cache identity, so
changing any option must change it.

### Installing Zig

The player installs Zig from the third setup step or from the hub's native code row (a worker thread around
`host_tc_install`, polled by the interface). The archive is the official one from ziglang.org: `tar.xz` on Linux,
`zip` on Windows; the file browser lists only that kind. An installation that finished setup before that step
existed is offered it once, on its own; `native-offered=1` in `<data>/gui-state.ini` records the offer. The
executable's identity and text are kept from boot (`host_native_boot`), so building starts as soon as an
installation finishes during a session. Discs added or removed in the hub during the game restart the service on
the new list (`host_native_discs`); modules already registered stay registered and the cache hands the rest over
again at once. The generator reads discs only through cue sheets, so a raw image gets a temporary sheet in
`<data>/native/` while the discs are read. Tests install a fixture archive by naming a manifest in
`LIONHEART_TOOLCHAIN_TEST_MANIFEST` (lines `version=`, `format=`, `prefix=`, `executable=`, `sha256=`, `size=`,
`file=` and `url=`; the format defaults to this system's), whose addresses must be on this machine, and hide the
system's compilers by giving the player a `PATH` without them.

The hub row shows how many sets are ready, failed sets or a stop with their details on demand
(`native_build_failures`: each failed set's name and the end of its diagnostics), and a time left. Measured on
full four-disc builds, the first tier is a few large sets that cost far more per instruction than the hundreds of
small effect sets after it, so a rate over the whole build said twice the real time left throughout and, in its
first minutes, jumped between one and sixty hours; and the last sets are mostly other discs' copies of code already
built, which settle at once. The estimate is therefore withheld until the first tier is done, then taken over the
last five minutes with units already compiled in unfinished modules counted, a set that has an earlier set's name,
address and size counted as a copy that costs nothing, and shown smoothed so that it counts down with the clock.
The OSD says once when the first tier is native; a launch that found it in the cache says nothing.

A compiler driver's own children do not get its death signal, and `zig cc`, `gcc` and `clang` all compile in
children of their own. Every compiler therefore announces its process group, from the child before it runs, on a
pipe to a small process the service forks when it starts; whatever ends the player closes that pipe, and the
process then kills every group still announced. On Windows a job object does the same.

The service lists the sets of the discs the player knows from the catalog alone (`lhg_plan_sets`, no disc read),
hands over every module the cache already has, and only then, if anything is missing, reads the discs once
(`lhg_session_open`) and emits one set at a time (`lhg_session_emit`) into a build folder. Worker threads compile
the units with the chosen compiler at lower priority, link the module, publish it and delete the sources and objects. Workers
are the processor count less two (one when there are fewer than four), and no more than available memory over
1.1 GiB. Order: the executable; sets whose code the guest has just entered without a native body; the inserted
disc's game sets (field, menus, world, battle and the rest of the non-effects); its battle effects; everything else.

```
<data>/native/<cache id>/in-use              held shared by every player using this folder
<data>/native/<cache id>/<exe id>/lock        held by the one player that builds
                                  include/    the runtime headers modules compile against
                                  modules/KEY.so (.dll) and KEY.ok, the receipt written after it
                                  sets/IDENTITY   the KEY that serves a set
                                  failed/IDENTITY attempts, KEY, the end of the compiler's diagnostics
                                  work/KEY/   a build in progress, kept to resume after a quit or crash
```

The cache id is a SHA-256 of the module ABI id, the generator fingerprint, the embedded catalog, the compiler (Zig by
its archive's checksum, a system compiler by its family and the version its macros give), the target and the
family's recipe: whatever changes generated code starts a new folder, and a folder no running player holds is
removed by the next builder. Modules from two compilers therefore never mix: a launch that
chooses another compiler builds into a new folder in the background, and the old folder goes with that build.
The executable id hashes the booted text. A set's IDENTITY hashes the catalog facts its
code comes from; a module's KEY hashes the code itself without the descriptor fields that name a set, so an overlay
whose code is the same on four discs, or under two catalog names, is compiled once and registered once.

A module is valid only with its receipt, and the receipt only if the file still has the recorded size. Publication
links the finished file into `modules/` without replacing anything, then writes the receipt and the set receipts by
rename. A compiled object counts only once the compiler has finished and it has been renamed into place, so a builder
starting up keeps `work/KEY/` folders of unpublished modules and resumes them; it removes anything else in `work/`,
any module without a receipt and any leftover temporary file, none of which has ever been loaded. `work/` is emptied
once everything is built. A second player on the same data folder loads published modules, checks for new ones
every two seconds, and becomes the builder when the first one exits. A set whose module fails to compile or is
refused by the loader is recorded in `failed/` with the end of the diagnostics (compiled without carets, so no
source text, and with build paths removed), retried once on a later launch, and then left interpreted. A failure
that is not the set's own (the discs, the disk, the compiler missing) stops building for that run and records
nothing against any set.

Save states record the precise host but not which code is native: the bit in the state's mode word that once meant
"interpreter only" is always set by the player, as it always was, so a state saved before a module arrived loads
after it and the other way round.

Hidden options drive the service headless: `--native-status` prints progress and a summary, `--native-workers N`,
`--native-off` keeps the interpreter, `--native-wait` lets the build finish after the guest stops,
`--native-build-only` builds without running the game, and `--native-compiler PATH` replaces the compiler driver.
`lionheart-gen --plan` and `lionheart-gen --emit-set INDEX|all` show the same plan and per-set keys.
