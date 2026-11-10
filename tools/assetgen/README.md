# Host build generators

The top-level build uses CMake 3.21 JSON support for public metadata and a C99
host executable for binary assets. No interpreter is needed. JSON conversion is
small enough for script mode; PNG inflation and font byte formatting belong in
the host executable. The zlib decoder is vendored from stb_image under MIT.

The metadata interfaces accept absolute or relative input and output paths:

```sh
cmake -DINPUT=games/ff8-usa -DOUTPUT=build/disc_identity.h -P tools/disc_identity_header.cmake
cmake -DINPUT=games/ff8-usa/movies.json -DOUTPUT=build/movie_map.inc -P tools/movie_map_header.cmake
```

The converter can be built independently of the runtime:

```sh
cmake -S tools/assetgen -B build/assetgen
cmake --build build/assetgen -j4
build/assetgen/assetgen gui . build/gui_assets.h
build/assetgen/assetgen spleen . build/osd_font.h
```

The first argument selects the conversion; the second is a source root with the
same asset layout as this repository. Fixture roots can supply synthetic fonts,
PNG images or BDF cells. The output directory must exist. Files retain the host newline convention of the legacy tools. The legacy
generator names in output comments preserve byte compatibility. The committed OSD font remains an input to the runtime; the build
does not regenerate it.

Native test projects can add this directory with `add_subdirectory` and run
`$<TARGET_FILE:assetgen>`, or use the standalone build above. Metadata fixtures
can invoke the scripts with their own `INPUT` directory or JSON file.
`test_disc_validation` and the identity-header fixtures in `test_host_player`
can use the metadata interface. `test_host_gui` can use the GUI interface.
`test_startup` and the synthetic-player fixtures still need the separate native
catalog generator; these replacements remove only their header dependency.
No tests are ported here.

For a cross-build, `host.cmake` configures a separate native CMake project and
uses its executable. `LH_HOST_C_COMPILER` selects a host compiler if automatic
host lookup is unsuitable. The target toolchain and target compile flags are
not passed to that project. The helper supports multi-configuration generators
by building its Release configuration at a fixed output path.

Python scripts remain available as parity oracles until their importing tests
are migrated. The Windows dependency fetcher, build driver, staging and
release scanner still need their separate ports. The driver relocates cached
pkg-config prefixes and stages dependency notices after building; its replacement
must preserve both steps and the pinned dependency identities. The developer
restoration ABI hashes the native generator fingerprint. The missing-catalog
diagnostic points to `cmake -P tools/setup.cmake -- --disc1 DISC`.

CI workflows are unchanged. The eventual CI switch removes setup-python and
pip, builds the native suite, and runs CTest with the existing tier selection.
Windows CI must call the replacement dependency/build/staging driver and retain
the pinned caches. That switch depends on the remaining ports.
