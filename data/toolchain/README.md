# Pinned player compiler

`manifest.json` is the reviewed download identity. CMake compiles the host entry
into `lionheart_toolchain`. The player never retrieves a remote manifest.
The component depends on zlib and the platform HTTP library, with no SDL or GUI.

The Windows entry pins the official Zig 0.17.1 ZIP. The Linux entry pins the
verified slim ZIP repack: 59,586,314 bytes, with the `zig-cc-slim/` prefix.
Its URL list is empty until a public repack host is selected. The upstream Linux
archive uses tar.xz, which this component does not support. A future xz reader
can use a separate manifest format. Updating an archive requires reviewing its
upstream authenticity, size, checksum, prefix, and retained files together.

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
ZIP entry names and types are checked even for omitted files. Only manifest
files are extracted. Extraction restarts in a fresh staging directory.

The self-check runs `zig cc --version`, compiles a C string to an object, and
links a shared library without the platform CRT. The receipt records successful
self-check completion, archive identity, and each installed file's SHA-256 and
size. It is the last staging file written. A rename publishes the whole stage
as `toolchain/<version>/`. Interrupted stages never count as installed.

The hidden developer option is `--toolchain-install [archive.zip]`.
`--toolchain-install --url URL` substitutes a fixture URL while preserving the
pinned size and checksum. Neither form appears in player help.

Native cases live in `tests/native/cases/test_toolchain.cpp`. The standalone
`tests/toolchain` CMake project builds the same contract executable without
unrelated player dependencies. With `LH_HOST_RUNNER` set to the native runner,
it registers the same four cases. Windows builds use the existing llvm-mingw
toolchain and Wine as `CMAKE_CROSSCOMPILING_EMULATOR`.
