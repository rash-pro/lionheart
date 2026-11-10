# Contributing to Lionheart

Lionheart is the whole port: the recompiler, the portable runtime, the host, the per-version metadata and the tests.
The recompiled C is generated on your machine from your own discs; it is never committed. Release players start on
the interpreter and build the same code for themselves in the background once their compiler is installed.

## Set up from your discs

You need the Linux build requirements from the [README](README.md). Setup uses CMake and a C99 host compiler.
Run setup from the repository root:

```sh
nice -n 10 cmake -P tools/setup.cmake -- --disc1 /path/to/disc1.cue --disc2 /path/to/disc2.cue --disc3 /path/to/disc3.cue --disc4 /path/to/disc4.cue --jobs 4
```

Only disc 1 is required. Give all four discs to generate the complete catalog. Setup does the following:
1. extracts the EXE from disc 1 and verifies it;
2. discovers the functions and generates C in `build/generated/`;
3. builds the developer executable `build/dev/ff8`, with the GUI, statistics, Vulkan and PGXP;
4. prints the command to run it. The developer executable shows the GUI only when it gets `--gui`.

Everything extracted or generated stays in `build/`.

Setup first builds the standalone `lionheart-gen` host tool in `build/generator/`. `--jobs` accepts 1 through 8
and defaults to 8. Use `--jobs 4` on a shared machine. The generator library owns the cache, so an embedding host
uses the same policy.

The cache fingerprints the native generator sources, catalog bytes, generation options and verified disc inputs.
Every run verifies each EXE's size and SHA-1 and the selected overlays against the versioned catalog.
A hit also checks every generated output with SHA-256. Missing or corrupt files cause regeneration.
The receipt is removed before generation and published atomically after all outputs close. Interrupted runs
and caches from the old setup are not reused. Setup takes an exclusive lock on `build/` for the whole operation.

The native generator also cross-builds with llvm-mingw without the host's SDL or Vulkan dependencies:

```sh
cmake -S generator -B build/generator-windows -DCMAKE_SYSTEM_NAME=Windows -DCMAKE_C_COMPILER=/path/to/llvm-mingw/bin/x86_64-w64-mingw32-clang
cmake --build build/generator-windows --target lionheart-gen -j4
```

On Windows, run `lionheart-gen.exe --disc disc1.cue --legacy-overlays --cache -o build/generated` from the
repository root. Repeat `--disc` for the other discs. Generation uses UTF-8 paths and binary writes on both systems.
The full setup command above builds the Linux developer host.

Generator fingerprints identify the save-state build.
Existing save states from the previous build are rejected. In-game memory card saves remain compatible.

To cross-build the Windows player on Linux, run `cmake -P tools/build_windows.cmake`.
Set `-DDEPS=/path/to/dependencies`, `-DBUILD=/path/to/build` and `-DJOBS=4` before `-P` to override the defaults.
Downloads are verified against `tools/windows-deps.json`; source markers and installed-file receipts reject foreign caches.
Set `-DOFFLINE=ON` to require cached inputs. An old `install-ucrt` folder without a receipt must be rebuilt.

Stage the Linux player with `cmake -P tools/stage_player.cmake`. This builds `build/linux/lionheart` and copies it
with its notices into `build/linux/dist-linux`. Use `-DBUILD=/path/to/build -DSTAGE_ONLY=ON` before `-P` to stage an
existing build. Run `cmake -DPLATFORM=linux -P tools/ci_deps.cmake` to check the CI build prerequisites.

## Tests

The native suite needs C++17 and the build dependencies listed in the README:

```sh
cmake -S tests/native -B build/native-tests
nice -n 10 cmake --build build/native-tests -j4
cmake -S . -B build/player
nice -n 10 cmake --build build/player --target ff8 -j4
FF8_PLAYER_HOST="$PWD/build/player/lionheart" ctest --test-dir build/native-tests -j4 --output-on-failure -LE 'game|realtime'
```

For the tier-2 boot checks after setup, set the input folders described below and run
`ctest --test-dir build/native-tests -R test_boot_integration --output-on-failure`.
The archive extraction tests need a temporary directory outside the checkout. Leave `TMPDIR` unset or point
it at an external scratch folder when running CTest.

The generator's standalone checks cover cache identity, output integrity and setup arguments:

```sh
nice -n 10 cmake --build build/generator -j4
ctest --test-dir build/generator --output-on-failure
```

With `LIONHEART_DISCS` set, `generator/test/setup_cache.sh` also checks real-disc hits, catalog and option
changes, source fingerprints, corruption, old receipts and a killed generation. Use a fresh
`build/setup-cache-check/` directory for each run.

**Tier 1** needs no game data. It runs synthetic inputs and small hand-written MIPS programs through discovery, C
emission, compilation and execution, and compares the results against the interpreter. CI runs this tier.

```sh
SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy ctest --test-dir build/native-tests --output-on-failure -LE 'game|realtime'
```

**Tier 2** uses your discs and the catalog from setup. Point `LIONHEART_DISCS` at the folder that holds your
`.cue` files; discs are recognised by serial. Without it, these tests skip and print the reason.

Some route tests also need two of your own memory cards, named `disc1-early.mcr` and `disc1-start-maxed.mcr`. Set
`LIONHEART_SAVES` to their folder, or put them in a `saves/` folder inside the discs folder. The tests work on
copies of the cards.

```sh
LIONHEART_DISCS=/path/to/discs LIONHEART_SAVES=/path/to/cards SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy ctest --test-dir build/native-tests --output-on-failure -L game
```

Tests marked `realtime` measure wall-clock pacing. CI skips them (`-LE 'game|realtime'`). If one fails on a
busy machine, run it again on its own.

Tier-1 Vulkan presentation tests use the Vulkan loader's device selection. They honor
`VK_DRIVER_FILES` (or the older `VK_ICD_FILENAMES`) instead of forcing lavapipe.
Select one ICD for each run; manifest filenames vary by distribution:

```sh
VK_DRIVER_FILES=/usr/share/vulkan/icd.d/nvidia_icd.json SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy ctest --test-dir build/native-tests --output-on-failure -LE 'game|realtime'
VK_DRIVER_FILES=/usr/share/vulkan/icd.d/lvp_icd.json SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy ctest --test-dir build/native-tests --output-on-failure -LE 'game|realtime'
```

Unset both variables to exercise the loader default. The host ranks compatible
discrete, integrated, virtual and CPU devices in that order; `GRANITE_VULKAN_DEVICE_INDEX`
overrides the enumerated device index. Startup logs identify the selected GPU.
With no usable default device the optional contracts skip; an unavailable explicitly
selected ICD fails the setup. The synchronization test skips if the Khronos validation
layer is missing and also looks in `VK_LAYER_PATH` and `VK_ADD_LAYER_PATH` for local
manifests. Test builds use at most four compilation jobs.

A connected controller can affect the SDL inventory tests; if one fails, include
your controller model in the report.

## Rules

- **No game data in git:** never commit discs, extracted files, generated C, game text, memory cards, dumps or
  movies. Hashes are fine as regression facts; byte arrays copied from the game are not fine as test fixtures.
- **Licences:** code must be MIT, ISC, BSD, zlib or MPL-2.0. No GPL code, and no code from DuckStation. Keep every
  upstream licence and source notice. The fonts are OFL, and FFmpeg stays a dynamically linked LGPL dependency.
- **Presentation changes must not change the game.** Anything that only affects how the game looks or sounds
  (renderer, PGXP, the GUI, filters) must leave the guest state and the native software frames identical. Build a
  baseline before your change, then compare:

  ```sh
  build/devtools/compare --before /path/to/baseline/ff8 --after build/dev/ff8 --exe build/generated/extracted/SLUS_008.92 --disc /path/to/disc1.cue --script tests/scripts/new-game.txt --vblanks 15000 --output build/exactness
  ```

  This compares the complete `--dump-state` and every software `--dump-frames` PNG. Run it again with
  `tests/scripts/profile-field.txt` for 2000 VBlanks with `--card disc1-start-maxed.mcr`, and with
  `tests/scripts/profile-battle.txt` for 2600 VBlanks with `--card disc1-early.mcr`.
- **The player must contain no game code.** Check it with the legal scanner:

  ```sh
  cmake -S tools/player-check -B build/player-check -DCMAKE_BUILD_TYPE=Release
  cmake --build build/player-check -j4
  build/player-check/check_player_binary build/lionheart build/generated/extracted/SLUS_008.92 --disc /path/to/disc1.cue --compile-commands build/compile_commands.json
  ```

  Repeat `--disc` for all four discs, so that it also scans every overlay. The native scanner reads the versioned
  complete legacy images and loader-bounded catalog images directly from your discs without writing extracted bytes.
  It checks source and include paths, nested response files, compiler forwarding options and symlinks.
  Repeat `--generated-dir PATH` for catalog roots not named in the build's `CMakeCache.txt`.

## Pull requests

Open an issue before starting a large change. Keep each branch focused. Describe the behaviour you changed and
include your test results. For runtime or recompiler changes, attach the exactness result and say which discs you
used, without attaching any game data. [docs/debugging.md](docs/debugging.md) explains how to track down a
divergence.

## Maintaining disc metadata

Build the standalone [developer tools](tools/devtools/README.md) and use
`build/devtools/ff8files import` with a pinned upstream checkout and all four
owned discs. Generate into an external scratch directory, verify with
`ff8files verify`, then review the metadata diff before copying JSON into
`games/ff8-usa`. Keep extracted bytes outside tracked source directories.
The metadata tool and its synthetic tests need no interpreter.
