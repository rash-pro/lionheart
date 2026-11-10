set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
  "${CMAKE_CURRENT_LIST_DIR}/../data/toolchain/manifest.json"
  "${CMAKE_CURRENT_LIST_DIR}/../tools/toolchain_manifest.cmake")
execute_process(COMMAND "${CMAKE_COMMAND}"
  "-DINPUT=${CMAKE_CURRENT_LIST_DIR}/../data/toolchain/manifest.json"
  "-DOUTPUT=${CMAKE_CURRENT_BINARY_DIR}/toolchain_manifest.c"
  "-DHOST_WINDOWS=${WIN32}" -P "${CMAKE_CURRENT_LIST_DIR}/../tools/toolchain_manifest.cmake"
  COMMAND_ERROR_IS_FATAL ANY)
add_library(lionheart_toolchain STATIC
  "${CMAKE_CURRENT_LIST_DIR}/../backends/host/toolchain.c"
  "${CMAKE_CURRENT_LIST_DIR}/../generator/src/sha256.c"
  "${CMAKE_CURRENT_BINARY_DIR}/toolchain_manifest.c"
  # The official Linux compiler archive is tar.xz.
  "${CMAKE_CURRENT_LIST_DIR}/../third_party/xz-embedded/xz_crc32.c"
  "${CMAKE_CURRENT_LIST_DIR}/../third_party/xz-embedded/xz_crc64.c"
  "${CMAKE_CURRENT_LIST_DIR}/../third_party/xz-embedded/xz_dec_lzma2.c"
  "${CMAKE_CURRENT_LIST_DIR}/../third_party/xz-embedded/xz_dec_stream.c")
target_include_directories(lionheart_toolchain PUBLIC "${CMAKE_CURRENT_LIST_DIR}/../backends/host"
  PRIVATE "${CMAKE_CURRENT_LIST_DIR}/../generator/src" "${CMAKE_CURRENT_LIST_DIR}/../third_party/xz-embedded")
target_compile_definitions(lionheart_toolchain PRIVATE XZ_USE_CRC64)
target_link_libraries(lionheart_toolchain PRIVATE ZLIB::ZLIB)
if(WIN32)
  target_link_libraries(lionheart_toolchain PRIVATE winhttp)
else()
  find_package(PkgConfig REQUIRED)
  pkg_check_modules(TOOLCHAIN_CURL REQUIRED IMPORTED_TARGET libcurl>=8)
  target_link_libraries(lionheart_toolchain PRIVATE PkgConfig::TOOLCHAIN_CURL)
endif()
target_compile_options(lionheart_toolchain PRIVATE -Wall -Wextra -Wpedantic)
