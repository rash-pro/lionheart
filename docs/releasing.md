# Release packages

A release is two files and their checksums: `Lionheart-VERSION-x86_64.AppImage` for Linux and
`Lionheart-VERSION-windows-x86_64.zip` for Windows, both x86-64, built from a tag by
[`.github/workflows/release.yml`](../.github/workflows/release.yml). Neither contains game code or a compiler.

## Cut a release

1. Set the version in `project(lionheart VERSION X.Y.Z)` in `CMakeLists.txt` and commit it on `main`.
2. Tag that commit `vX.Y.Z` and push the tag. A tag that does not name the version in `CMakeLists.txt` stops the
   workflow at its first job.
3. The workflow builds both packages in pinned images, checks them (`tools/check_package.cmake`), starts the
   AppImage without discs (`packaging/linux/test-appimage.sh`) and the zip on a hosted Windows runner
   (`tools/smoke_package.cmake`), and attaches both with `SHA256SUMS` to a **draft** release.
4. Read the draft, write its notes, and publish it.

Run the workflow again on an existing tag from the Actions tab (*Release packages*, *Run workflow*, the tag). It
replaces the files of a draft and refuses to touch a published release.

## What the packages hold

The AppImage is the statically linked AppImage type2-runtime (upstream commit `fedfad51acc2860b2916a1eef32ba4a500ff31ef`) followed by a zstd-compressed squashfs image: `AppRun`, the desktop entry and icon,
`usr/bin/lionheart`, the libraries below in `usr/lib`, the hicolor icons and `usr/share/doc/lionheart/` with the
notices and the dependency lock. The player finds its libraries through its run path (`$ORIGIN/../lib`), never
`LD_LIBRARY_PATH`, and `AppRun` removes the four variables the runtime sets before it starts the player, so every
program the player starts, a compiler included, sees the environment the user launched it with.

| Bundled in the AppImage | Licence | Notice in `usr/share/doc/lionheart/licences/` |
| --- | --- | --- |
| SDL2 2.30.9 (`libSDL2-2.0.so.0`) | zlib | `SDL2-zlib.txt` |
| FFmpeg 7.1.1 (`libavformat`, `libavcodec`, `libavutil`, `libswscale`) | LGPL-2.1-or-later, shared | `FFmpeg-LGPL-2.1.txt`, `FFmpeg-configure.txt`, `FFmpeg-source.txt` |
| libcurl 8.22.0 (`libcurl.so.4`), HTTP and HTTPS only | curl (MIT-style) | `curl.txt` |
| zlib 1.3.1, linked into the player | zlib | `zlib.txt` |
| AppImage type2-runtime (944,632-byte launcher) | MIT, BSD-2-Clause, BSD-3-Clause, zlib; libfuse LGPL-2.1-or-later | `AppImage-runtime.txt`, `AppImage-LGPL-2.1.txt`, `AppImage-source.txt` |
| Lionheart and its vendored code | MIT, ISC, BSD-2-Clause, zlib, OFL-1.1 | as in the Windows folder |

From the system it needs glibc 2.35 or later, libstdc++ (GLIBCXX 3.4.30, from GCC 12), OpenSSL 3 (`libssl.so.3`,
`libcrypto.so.3`), D-Bus (`libdbus-1.so.3`), and at run time the display, audio and input libraries SDL2 opens,
the Vulkan loader and driver, and kernel FUSE support to mount the image. No system FUSE library is needed; where mounting is forbidden, use `--appimage-extract-and-run`.

The zip holds one folder, `Lionheart`, with exactly what `tools/build_windows.cmake` stages: `Lionheart.exe`, SDL2,
the FFmpeg DLLs, the llvm-mingw C++ runtime and winpthreads, `dependencies.json` and `licences/`.

## Build them yourself

Each package is built in an Ubuntu image pinned by digest, from Ubuntu's archive as it was on a fixed date
(`packaging/build-environment.sh`), from sources pinned by SHA-256 and size in `tools/linux-deps.json` and
`tools/windows-deps.json`. Two builds of the same tag give the same bytes. With Docker, from a clone at the tag:

```sh
docker run --rm -v "$PWD:/src" -w /src \
  ubuntu:22.04@sha256:076e7bf82319619f28dd0aa9c02ab34b055ad37100395b4f49a2ffba04fb5e11 \
  sh -c 'sh packaging/build-environment.sh linux && git config --global --add safe.directory /src &&
         cmake -DJOBS=4 -P tools/build_appimage.cmake'
docker run --rm -v "$PWD:/src" -w /src \
  ubuntu:24.04@sha256:fe81560990fae86dc4743202b2cb4d3df98d0bcf949173cecef15ce1127ef852 \
  sh -c 'sh packaging/build-environment.sh windows && git config --global --add safe.directory /src &&
         cmake -DJOBS=4 -P tools/build_windows.cmake && cmake -P tools/package_windows.cmake'
sha256sum build/packages/*
```

Compare the result with `SHA256SUMS`. The build reads its file dates from the tagged commit, keeps the checkout's
and the build folders' names out of the binaries, and the zip and squashfs writers fix order, dates, owners and
modes.

`cmake -DPACKAGE=build/packages/FILE -P tools/check_package.cmake` checks either package: only expected files, every
library it loads bundled or a system one, an accepted licence and the notices for every bundled component, and the
version and icon. `-DSCANNER=…/check_player_binary -DEXE=…` adds the release scan
([tools/player-check](../tools/player-check/README.md)).

## Change a pin

A new source version goes into the lock with its SHA-256 and size, checked against what its publisher states. FFmpeg
and zlib must stay the same in both locks; the AppImage build refuses to run otherwise. A new image digest or archive
date goes into `packaging/build-environment.sh`, the workflow and the commands above together.

## Runtime and compiler

The launcher is the upstream type2-runtime continuous build at commit
`fedfad51acc2860b2916a1eef32ba4a500ff31ef`, pinned by SHA-256
`617aa77314d57c8b8e4fae9c3f0b35af7b30d5529cb69e98f64f92510ed98a46`
in `tools/linux-deps.json`. Upstream may replace continuous assets: a different
hash must fail the build; retain the verified download for reproducible rebuilds.
Its statically linked libfuse is an accepted LGPL exception for a separate launcher,
as FFmpeg is for shared libraries. Its licence texts and corresponding source and
relink instructions ship in `usr/share/doc/lionheart/licences/AppImage-*`.

On Linux the player probes Clang and GCC on the user's PATH. If neither works,
it downloads the official Zig tar.xz from ziglang.org and decodes it with the
vendored xz decoder. No compiler or game data ships in either package.
Check the packaged version with `./Lionheart-VERSION-x86_64.AppImage --version`.
The desktop entries accept `%f` and declare `application/x-cue`, as the installed
player does; the desktop MIME cache is generated inside the package prefix.

The no-disc check is `sh packaging/linux/test-appimage.sh PACKAGE`; it also checks
the untouched launch environment. For actual compiler processes, compile
`packaging/linux/compiler-environment.c` in the target distribution and run
`sh packaging/linux/test-compiler-environment.sh PACKAGE DISC.cue WORK WRAPPER`.
Add `zig` after installing the official compiler into `WORK/data` with
`XDG_DATA_HOME=WORK/data PACKAGE --toolchain-install`, on a system without Clang
or GCC. The test compares every compiler environment and requires a completed
first tier. `sh packaging/linux/test-package-checker.sh PACKAGE` proves that
missing notices, unknown libraries and incorrect versions are rejected.
