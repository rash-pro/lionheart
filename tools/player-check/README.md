# Check a release player

Build this host tool separately from the player. It needs a C99 compiler, a C++17 compiler and CMake 3.21.

```sh
cmake -S tools/player-check -B build/player-check -DCMAKE_BUILD_TYPE=Release
cmake --build build/player-check -j4
build/player-check/check_player_binary build/linux/lionheart /path/to/owned/SLUS_008.92 \
  --disc /path/to/disc1.cue --compile-commands build/linux/compile_commands.json
```

Repeat `--disc` for all four discs. The tool reads their CUE and raw MODE2/2352 images in place.
It uses the checked-in USA metadata to verify the EXE, complete legacy overlays, stored catalog images and
loader-bounded catalog images. It preserves catalog deduplication and set ordering, including effects and
ambiguous effect-address rejection. Function discovery and C emission are unnecessary for a byte scan.
No extracted disc bytes or window fingerprints are written to disk.

The output is the same JSON receipt as the existing scanner. Each qualifying window has 64 bytes and at least
eight distinct byte values. The rolling hash indexes candidates; every match and every deduplication uses exact
bytes. Findings report the first binary offset. A missing or empty overlay extraction fails the scan.

Compiler auditing resolves symlinks and Windows separators. It checks named generated roots, `GENERATED_DIR`
from the adjacent CMake cache, source names, source arguments, every supported include option, nested response
files, `-Xclang` and `-Wp,` forwarding. Missing, unreadable, cyclic or over-deep response files fail the scan.
Use `--generated-dir PATH` for each additional catalog root. `--extract-dir PATH` selects the temporary-directory
parent for callers that use the existing interface; native extraction keeps the images in memory.

The other release operations use CMake script mode because CMake already supplies downloads, checksums,
archive extraction, process execution and file staging. C++ handles the byte and compiler-input scans without
turning those algorithms into CMake string processing. These tools add no dependencies.

Run the synthetic offline download-cache contracts with `cmake -P tools/test_release_tools.cmake`.
They check extraction, cache reuse, source provenance, archive digests, archive sizes and missing inputs.
The scanner's existing test identities are in `tests/native/cases/test_player_legal.cpp`.
