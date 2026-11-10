# Pinned player compiler

`manifest.json` is the reviewed download identity. CMake compiles the host entry
into `lionheart_toolchain`. The player never retrieves a remote manifest.
The component depends on zlib, XZ Embedded and the platform HTTP library, with no SDL or GUI.

Both entries pin the official Zig 0.17.1 archives as ziglang.org publishes them:
`zig-x86_64-linux-0.17.1.tar.xz` (57,411,036 bytes) and `zig-x86_64-windows-0.17.1.zip`
(100,343,666 bytes), downloaded from ziglang.org itself. Their sizes and SHA-256 values are the ones in
https://ziglang.org/download/index.json. Updating an archive means reviewing its authenticity (the
publisher's index and minisign signature), size, checksum, top folder (`prefix`) and retained files together.

`host_tc_pinned()` returns the host manifest. `host_tc_query(data, manifest)`
returns absent, partial, or installed. Installed requires a matching receipt
and matching contents for every retained file. These calls perform no HTTP.
`host_tc_executable` returns the validated compiler path.

`host_tc_install` runs synchronously on a caller-owned worker thread. A null
archive selects HTTP; an archive path selects offline verification with the
same pinned checksum. The callback receives phase, completed bytes or entries,
and total. Returning nonzero cancels. The caller copies progress to the UI
thread. `host_tc_error.reason` contains the failure reason; retry calls the same
function. Each operation owns its error and callback context.

The installer locks the data folder's toolchain directory. A competing installer
gets a retryable busy error. Download prefixes survive cancellation and short
responses. A complete download must match size and SHA-256 before extraction.
Only manifest files are extracted, and every entry is checked first, omitted
ones included: a name that leaves the destination, a link or a device refuses
the whole archive. ZIP is read through its central directory. `tar.xz` is read
as one xz stream (LZMA2, any check XZ Embedded verifies, here CRC64) holding a
ustar or GNU tar, with GNU long names and pax `path` and `size` records; the
stream must end where the tar's zero blocks do. Extraction restarts in a fresh
staging directory.

The self-check runs `zig cc --version`, compiles a C string to an object, and
links a shared library without the platform CRT. The receipt records successful
self-check completion, archive identity, and each installed file's SHA-256 and
size. It is the last staging file written. A rename publishes the whole stage
as `toolchain/<version>/`. Interrupted stages never count as installed.

The hidden developer option is `--toolchain-install [archive]`.
`--toolchain-install --url URL` substitutes a fixture URL while preserving the
pinned size and checksum. Neither form appears in player help. For the
interface's own tests, `LIONHEART_TOOLCHAIN_TEST_MANIFEST` names a whole
fixture manifest (`backends/host/native_setup.h`) whose addresses must be on
this machine.

Native cases live in `tests/native/cases/test_toolchain.cpp`; the tar.xz cases
build their archives with `tests/native/support/tar_xz.h`, which stores LZMA2
chunks uncompressed and needs no encoder. The standalone
`tests/toolchain` CMake project builds the same contract executable without
unrelated player dependencies. With `LH_HOST_RUNNER` set to the native runner,
it registers the same cases. Windows builds use the existing llvm-mingw
toolchain and Wine as `CMAKE_CROSSCOMPILING_EMULATOR`.
