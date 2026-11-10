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
PNG images or BDF cells. The output directory must exist. Files use the host newline convention and identify the assetgen command in output comments. The committed OSD font remains an input to the runtime; the build
does not regenerate it.

Native test projects can add this directory with `add_subdirectory` and run
`$<TARGET_FILE:assetgen>`, or use the standalone build above. Metadata fixtures
can invoke the scripts with their own `INPUT` directory or JSON file.
`test_disc_validation` and the identity-header fixtures in `test_host_player`
can use the metadata interface. `test_host_gui` uses the GUI interface.

For a cross-build, `host.cmake` configures a separate native CMake project and
uses its executable. `LH_HOST_C_COMPILER` selects a host compiler if automatic
host lookup is unsuitable. The target toolchain and target compile flags are
not passed to that project. The helper supports multi-configuration generators
by building its Release configuration at a fixed output path.
