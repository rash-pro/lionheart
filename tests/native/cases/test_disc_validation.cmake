include(${CMAKE_CURRENT_LIST_DIR}/../support/synthetic_disc.cmake)
foreach(name identify runtime-identify)
  set(extra)
  if(name STREQUAL runtime-identify)
    set(extra -DFF8_DISC_RUNTIME_TEST -DFF8_DISC_ALLOC_TEST)
    set(runtime runtime/cdrom.c)
    set(wrap -Wl,--wrap=malloc)
  endif()
  lh_harness(${name} FLAGS -std=c99 -O2 -Wall -Wextra -Wpedantic -Werror ${extra}
    -ffunction-sections -fdata-sections INCLUDES runtime backends/host
    SOURCES tests/disc_id_harness.c backends/host/disc_id.c backends/host/disc_iso.c runtime/cpu.c ${runtime}
    LINK_FLAGS -Wl,--gc-sections ${wrap})
  if(TARGET test_disc_validation.${name})
    target_include_directories(test_disc_validation.${name} PRIVATE "${LH_SYNTHETIC_DIR}")
  endif()
endforeach()
