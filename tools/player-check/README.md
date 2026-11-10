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
loader-bounded catalog images. Battle-stage code prefixes from `overlays.json` are scanned as well, each
variant's code checked against its recorded SHA-1. It preserves catalog deduplication and set ordering, including effects and
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

## Compiler release assets

The player pins trimmed Zig ZIPs in `data/toolchain/manifest.json`. Official input
names and SHA-256 values are reviewed separately in `data/toolchain/upstream.json`.
Obtain those official Zig 0.17.1 archives from ziglang.org and verify upstream
Minisign signatures when updating the pins. To reproduce either release asset:

```sh
cmake -DPLATFORM=x86_64-linux -DINPUT=/path/to/zig-x86_64-linux-0.17.1.tar.xz \
  -DWORK=/outside/checkout/fresh-linux -DOUTPUT=/output/zig-0.17.1-x86_64-linux-cc.zip \
  -P tools/toolchain_release.cmake
```

Repeat with `x86_64-windows`, its official `.zip` input and an independent fresh
work directory. A host C compiler and zlib development files are required. The
script verifies input SHA-256 before extracting only the selected files. It checks
the output size and hash against the player manifest; use `-DVERIFY=OFF` only
while preparing new pins, then repeat with verification enabled.

`tools/toolchain-zip` writes ZIP32 entries compressed with zlib at level 9, in sorted
path order, with a fixed DOS date of 1980-01-01, no extra fields or comments, and
Unix regular-file permissions 0644 (0755 for the compiler). No source metadata,
locale or timezone enters the bytes, so two builds with the same zlib are identical.
The compressed bytes can differ between zlib versions and implementations; the
pinned archives were written with zlib 1.3.2. With another zlib, check a published
asset by content instead: extract it and the archive you built and compare the
trees. Input bytes are copied unmodified. `LICENSE`, the LLVM text at
`lib/libcxx/LICENSE.TXT`, and the added provenance `NOTICE.txt` are included and
installed beside the compiler.

Upload `zig-0.17.1-x86_64-linux-cc.zip` and
`zig-0.17.1-x86_64-windows-cc.zip` as assets of this repository's
`toolchain-zig-0.17.1` GitHub release after checking both manifest hashes. The
builder itself creates no tag or release and performs no upload. Build each in
two fresh work directories and compare the ZIPs before publishing.
