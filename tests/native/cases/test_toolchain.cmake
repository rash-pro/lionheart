find_package(ZLIB REQUIRED)
include("${LH_ROOT}/cmake/toolchain.cmake")
lh_harness(install SOURCES tests/toolchain_harness.c generator/src/sha256.c
  INCLUDES backends/host generator/src FLAGS -std=c99 LIBS lionheart_toolchain)
