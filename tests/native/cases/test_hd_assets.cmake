find_package(ZLIB REQUIRED)
find_package(Threads REQUIRED)
include(${LH_ROOT}/tools/assetgen/host.cmake)
set(hd_assets "${CMAKE_BINARY_DIR}/hd-font-assets")
file(MAKE_DIRECTORY "${hd_assets}")
add_custom_command(OUTPUT "${hd_assets}/hd_font_assets.h"
  COMMAND ${ASSETGEN_COMMAND} hdfont "${LH_ROOT}" "${hd_assets}/hd_font_assets.h"
  DEPENDS ${ASSETGEN_DEPENDENCY} "${LH_ROOT}/third_party/barlow/Barlow-Medium.ttf" VERBATIM)
lh_harness(hd_font
  CXX_FLAGS -std=c++17 -O1 -Wall -Wextra -Werror
  INCLUDES backends/host third_party/imgui third_party/stb
  SOURCES tests/hd_assets_harness.cpp backends/host/hd_font.cpp "${hd_assets}/hd_font_assets.h")
target_include_directories(test_hd_assets.hd_font PRIVATE "${hd_assets}")
lh_harness(hd_models
  CXX_FLAGS -std=c++17 -O1 -Wall -Wextra -Wpedantic -Werror
  INCLUDES backends/host third_party third_party/stb
  SOURCES tests/hd_models_harness.cpp backends/host/hd_models.cpp
  LIBS ZLIB::ZLIB)
lh_harness(mod_import
  CXX_FLAGS -std=c++17 -O1 -Wall -Wextra -Wpedantic -Werror
  INCLUDES backends/host third_party third_party/stb
  SOURCES tests/mod_import_harness.cpp backends/host/dds_decode.cpp backends/host/ff8_mod_import.cpp
  LIBS ZLIB::ZLIB Threads::Threads)
