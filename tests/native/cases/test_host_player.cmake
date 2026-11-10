include(${CMAKE_CURRENT_LIST_DIR}/../support/synthetic_disc.cmake)
add_compile_definitions(LH_SYNTHETIC_METADATA="${LH_SYNTHETIC_DIR}")
lh_harness(identify FLAGS -std=c99 -O2 -Wall -Wextra -Wpedantic -Werror
  -ffunction-sections -fdata-sections "-I${LH_SYNTHETIC_DIR}"
  INCLUDES runtime backends/host
  LINK_FLAGS -Wl,--gc-sections
  SOURCES tests/disc_id_harness.c backends/host/disc_id.c backends/host/disc_iso.c runtime/cpu.c)
