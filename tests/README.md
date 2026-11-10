# Writing and running tests

The suite uses CTest and a C++17 runner. Build the player separately and pass its path:

```sh
cmake -S . -B build/player
nice -n 10 cmake --build build/player -j4
cmake -S tests/native -B build/native-tests
nice -n 10 cmake --build build/native-tests -j4
FF8_PLAYER_HOST="$PWD/build/player/lionheart" ctest --test-dir build/native-tests -j4 --output-on-failure -LE 'game|realtime'
```

Keep `TMPDIR` outside the checkout; archive extraction tests reject repository outputs.
Set `SDL_VIDEODRIVER=dummy` and `SDL_AUDIODRIVER=dummy` for headless runs.
`lionheart_tests --list` lists cases and tiers. CTest labels select `standard`, `game`, `realtime`, or `infra`.
The `game` tier needs `LIONHEART_DISCS`, a directory containing your disc cues; some routes also need
`LIONHEART_SAVES` with `disc1-early.mcr` and `disc1-start-maxed.mcr`. Missing inputs skip with a reason.
The `realtime` tier measures wall-clock pacing; run it separately on an idle machine.
The `infra` tier checks the runner and helpers and is excluded from baseline comparisons.

## Layout and case names

`native/cases/test_X.cpp` defines cases; `test_X.cmake` declares their harnesses.
`tests/*.c` and `tests/*.cpp` hold reusable C/C++ harnesses. `tests/scripts` holds input routes.
`native/support` contains process, library, generator, network and rendering fixtures.
`native/oracles` contains independent reference equations and image readers.
Case files and CMake fragments are discovered automatically on reconfiguration.

Names have the form `test_X::function[parameter]`. Use `LH_TEST(function)` for a standard case,
`LH_GAME_TEST(function)` for an owned-game case, and `LH_TEST_P(function, rows)` for parameters.
Rows may be strings or structures with an `id` member; an optional `tier` member selects a tier per row.
Keep file-local types and helpers in an anonymous namespace to avoid link-time collisions.

`identities.tsv` is the frozen coverage inventory: original node names, tiers and baseline outcomes.
The runner normalizes its historical filename suffix when comparing names.
Run `lionheart_tests --check-identities test_X` to check existing coverage and tiers.
Unknown stems are errors. New coverage need not rewrite this historical inventory.
The `parity` build target runs baseline comparisons; `LH_PARITY_STEMS` restricts it to semicolon-separated stems.

## Harnesses and helpers

Declare a harness with `lh_harness(name SOURCES tests/harness.c FLAGS -std=c99 -O1 INCLUDES runtime)`.
`RUNTIME_ALL` includes runtime sources; `RUNTIME_EXCLUDE` removes named sources.
`LIBRARY` builds a loadable library. `LIBS`, `LINK_FLAGS`, `CXX_FLAGS` and `SANITIZE_FLAGS` control compilation.
`FF8_TEST_SANITIZE=ON` enables optional sanitizer flags, including generator instrumentation.
Assertions remain enabled in every build type.

Use `t.harness(name)`, `t.player()` or `t.target(path)` for target executables; `t.host(path)` runs host tools.
Commands support `.arg`, `.cwd`, `.env`, `.unset`, `.input` and `.timeout`.
`t.ok(command)` requires success; `t.run(command)` returns status and output for explicit checks.
`t.temp()` supplies a fresh directory, retained on failure. `lh::skip(reason)` reports unavailable prerequisites.
Use `LH_EQ`, `LH_CHECK_MSG`, `LH_CONTAINS`, `LH_BYTES_EQ`, `LH_TEXT_EQ` and `LH_FILE_EQ` for readable failures.
Pass paths as `lh::fs::path` so target commands receive Windows paths under Wine.

`t.library(name).fn<Signature>(symbol)` loads harness functions. Each case runs in its own process.
Library cases skip for Windows cross-targets because a Linux runner cannot load Windows DLLs.
Cross-configure with the Windows toolchain and `-DLH_HOST_RUNNER=<native build>/lionheart_tests`;
set `CMAKE_CROSSCOMPILING_EMULATOR` to Wine for executable harnesses.

`lh::PyRandom` reproduces integer-seeded CPython random sequences. Preserve draw order;
never put multiple draws in arguments whose evaluation order is unspecified.
`NumpyRandom` supplies SeedSequence/PCG64 byte fills. Independent oracles must not link or copy the code they judge.
`vulkan.h` builds isolated rendering contracts and honors `VK_DRIVER_FILES` and `VK_ICD_FILENAMES`.
An unusable default device skips; an explicitly selected unavailable device fails. Synthetic clips need FFmpeg.
`player_probe.h` relinks an existing player from its compile receipt; owned-game probes need discs and a card.

## Generated programs and reference output

`support/generated.h` provides MIPS assemblers, `Image`, `Program`, `Discovery` and `Build`.
Construct a program, then use `Build(t).program(program).runtime_all().library()` to emit and compile it.
`.probe(file, replacements)` substitutes template text; `.source`, `.flags` and `.header_prefix` customize the build.
`generate(t, program, directory)` emits without compilation. Generator internals are exposed through `generator_shim.h`.

Builds share a locked content-addressed cache under the native build directory. Its key includes programs,
flags, sources, runtime, generator and compiler identity. `LH_COMPILE_JOBS` defaults to two.
Delete the cache to force recompilation. Keep program construction in the case so input logic has one owner.

`test_generator_parity` and `test_generator_rules` check complete output trees against
`native/generator_reference.tsv`, with SHA-256 per identity, subcase and output file. Parameters of the
former live in `cases/generator_parity_rows.h`; shared helpers in `cases/generator_cases.h`.
To accept an intentional generator change:

```sh
cmake -DBUILD=build/native-tests -P tests/native/rewrite_generator_reference.cmake
```

This accepts current output. Review the entire manifest diff before committing it.

`test_metadata_tool` covers native disc metadata commands with synthetic ISO
sectors and checks the supported upstream YAML subset. No interpreter is needed
for the developer tools or this suite.

## Run the Windows target on Linux

Set `TMPDIR` to a directory outside the checkout. Install Wine and the host build dependencies,
then build the player, the Linux runner, and the Windows tests:

```sh
cmake -DPLATFORM=windows -P tools/ci_deps.cmake
nice -n 10 cmake -DJOBS=4 -P tools/build_windows.cmake
cmake -S tests/native -B build/native-tests
nice -n 10 cmake --build build/native-tests --target lionheart_tests -j4
nice -n 10 cmake -DJOBS=4 -P tools/build_windows_tests.cmake
export WINEDEBUG=-all DISPLAY= SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy
export WINEPATH="$PWD/build/windows/dist-windows"
export FF8_PLAYER_HOST="$PWD/build/windows/Lionheart.exe"
wine cmd /c exit 0
nice -n 10 ctest --test-dir build/windows-tests -j4 --output-on-failure -LE 'game|realtime'
```

Windows tests use the pinned SDL2 and zlib dependencies. The HTTP backend uses WinHTTP.
The Linux runner skips DLL calls. Cases that need ELF linker features, POSIX helpers,
or an interactive display report their missing prerequisite. File size limit failures
skip at the point that requires `setrlimit`; other memory card cases still run.
Use `fs::path` values for path arguments and path environment variables.
Harness streams use binary mode to preserve input bytes and line endings.

## Run the relocated bundle

Build the Windows tests first, then create and extract the bundle in another directory:

```sh
cmake -P tools/bundle_windows_tests.cmake
cmake -E make_directory build/relocated-windows-tests
cmake -E chdir build/relocated-windows-tests cmake -E tar xf ../windows-test-bundle.zip
nice -n 10 cmake -DEMULATOR=wine -P build/relocated-windows-tests/run_windows_bundle.cmake
```

On Windows, extract the same archive and run `cmake -P run_windows_bundle.cmake` from its directory.
The script checks the player's headless no-disc diagnostic, then runs the Windows runner with `--suite`.
Each executable case runs in a separate process with a deadline. The suite excludes `game` and `realtime`.
The Windows runner retains the Linux inventory. Cases whose host fixtures need POSIX or an on-demand
compiler remain registered with a skip reason. Portable cases use the original case bodies and load Windows DLLs.
The workflow marks the hosted Windows job as pending verification until it has run there.
