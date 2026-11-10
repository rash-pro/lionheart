# Third-party notices

No game code or assets are included. The following components accompany the
source, the Linux AppImage or the Windows distribution. Vendored sources retain their notices;
`SOURCE.md` files record upstream revisions and local changes.

| Component | Licence and shipped notice |
| --- | --- |
| JSON for Modern C++ 3.12.0 (metadata tool only) | MIT: [LICENSE.MIT](third_party/nlohmann/LICENSE.MIT) |
| stb_image 2.30 (host build only) | MIT: [stb_image.h](third_party/stb/stb_image.h); standalone zlib decoder |
| parallel-psx | ISC: [COPYING](third_party/parallel-psx/COPYING) |
| Vulkan/util helpers in parallel-psx | MIT notices in the individual source headers |
| volk | MIT: [volk.h](third_party/parallel-psx/volk/volk.h) |
| rcheevos | MIT: [LICENSE](third_party/rcheevos/LICENSE); MD5 also retains its zlib-style notice in [md5.c](third_party/rcheevos/src/util/md5.c) |
| libcurl 8.22.0 (Linux; bundled in the AppImage, HTTP and HTTPS only) | curl licence (MIT-style): [curl.txt](licenses/curl.txt) |
| Spleen 2.2.0 | BSD-2-Clause: [LICENSE](third_party/spleen/LICENSE); compiled OSD cells |
| XZ Embedded v2024-12-30 | 0BSD: [COPYING](third_party/xz-embedded/COPYING); decodes the Linux compiler archive |
| Dear ImGui 1.91.9b | MIT: [LICENSE.txt](third_party/imgui/LICENSE.txt) |
| nativefiledialog-extended 1.2.1 | zlib: [LICENSE](third_party/nativefiledialog-extended/LICENSE) |
| Chakra Petch | OFL-1.1: [licence text](third_party/chakra-petch/OFL.txt); GUI fonts and logo typography |
| SDL2 2.30.9 | zlib: [SDL2-zlib.txt](licenses/SDL2-zlib.txt) |
| FFmpeg 7.1.1 | LGPL-2.1-or-later: [FFmpeg-LGPL-2.1.txt](licenses/FFmpeg-LGPL-2.1.txt) |
| zlib 1.3.1 | zlib: [zlib.txt](licenses/zlib.txt) |
| llvm-mingw runtime | LLVM Apache-2.0 with LLVM exception: [llvm-mingw-runtime.txt](licenses/llvm-mingw-runtime.txt) |
| MinGW-w64 / winpthreads | Public domain / ZPL notices: [COPYING.MinGW-w64-runtime.txt](licenses/COPYING.MinGW-w64-runtime.txt) and [COPYING.winpthreads.txt](licenses/COPYING.winpthreads.txt) |
| AppImage type2-runtime fedfad51 | MIT, BSD-2-Clause, BSD-3-Clause, zlib; statically linked libfuse LGPL-2.1-or-later: [licences](licenses/AppImage-runtime.txt), [LGPL](licenses/AppImage-LGPL-2.1.txt), [source and relinking](licenses/AppImage-source.txt) |

FFmpeg is dynamically linked. The exact unmodified corresponding source is
https://ffmpeg.org/releases/ffmpeg-7.1.1.tar.xz (SHA-256 and size are locked in
[windows-deps.json](tools/windows-deps.json)). The shipped build disables GPL,
nonfree, version3 and network components. `licences/FFmpeg-configure.txt`
records its complete configure command; `licences/FFmpeg-source.txt` accompanies
the DLLs. Rebuild with `cmake -P tools/build_windows.cmake`. Users may replace the
shared libraries with compatible modified versions.

SDL2, zlib and the compiler's exact source/archive URLs, versions and SHA-256
values are also recorded in that lock file. The Linux AppImage builds SDL2, zlib,
libcurl and the same FFmpeg from the sources pinned in
[linux-deps.json](tools/linux-deps.json), with `tools/build_appimage.cmake`; its
FFmpeg configure line and source notice are in `usr/share/doc/lionheart/licences/`
inside it, and its libraries can be replaced after `--appimage-extract`. A player
built from source on Linux uses the installed system libraries and their
distribution's licence and source packages.

The RetroAchievements transport uses system libcurl on Linux and the Windows WinHTTP system API on Windows.
The badge decoder uses the existing zlib dependency; no additional image library is shipped.

The player does not contain a compiler. To build the game's native code it uses a C compiler the system already
has (Clang or GCC on Linux) when one works; nothing of it is copied or shipped. Otherwise, when the user starts that
step, it downloads the official Zig 0.17.1 archive from [ziglang.org](https://ziglang.org/download/) and installs
only the files it needs in the user's data folder. Zig is MIT-licensed and includes LLVM under Apache-2.0 with LLVM
exceptions; both licence texts (`LICENSE` and `lib/libcxx/LICENSE.TXT`) are among the installed files.
