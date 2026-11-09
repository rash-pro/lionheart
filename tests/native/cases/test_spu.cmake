set(sources ${LH_RUNTIME_SOURCES})
list(FILTER sources EXCLUDE REGEX "/spu[.]c$")
lh_harness(main FLAGS -std=c99 -O1 -g -Wall -Wextra -Werror
  INCLUDES runtime SANITIZE_FLAGS ${LH_SANITIZE}
  SOURCES tests/spu_harness.c ${sources})
find_package(PkgConfig REQUIRED)
pkg_check_modules(SPU_SDL REQUIRED IMPORTED_TARGET sdl2)
lh_harness(audio FLAGS -std=c99 -O1 -g -Wall -Wextra -Werror
  INCLUDES runtime SANITIZE_FLAGS ${LH_SANITIZE}
  SOURCES tests/audio_harness.c runtime/spu.c)
target_link_libraries(test_spu.audio PRIVATE PkgConfig::SPU_SDL)
