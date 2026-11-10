include("${CMAKE_CURRENT_LIST_DIR}/../support/network.cmake")
find_package(PkgConfig REQUIRED)
if(CMAKE_SYSTEM_NAME STREQUAL "Windows")
  pkg_check_modules(LH_RA REQUIRED sdl2)
  find_package(ZLIB REQUIRED)
  list(APPEND LH_RA_CFLAGS "-I${ZLIB_INCLUDE_DIRS}")
  list(APPEND LH_RA_LDFLAGS ${ZLIB_LIBRARIES} winhttp)
else()
  pkg_check_modules(LH_RA REQUIRED sdl2 libcurl zlib)
endif()
set(vendor third_party/rcheevos)
set(sources)
foreach(part runtime client api)
  file(GLOB part_sources CONFIGURE_DEPENDS "${LH_ROOT}/${vendor}/src/${part}/*.c")
  list(SORT part_sources)
  list(APPEND sources ${part_sources})
endforeach()
foreach(source util/md5.c util/rc_compat.c util/rc_util.c util/rc_version.c hash/rc_hash.c hash/rc_hash_disc.c hash/rc_cdreader.c)
  list(APPEND sources ${vendor}/src/${source})
endforeach()
lh_harness(client LIBRARY
  FLAGS -std=c99 -O1 -Wall -Wextra -Wpedantic -Werror -D_DEFAULT_SOURCE -D_POSIX_C_SOURCE=200809L
        -D_FILE_OFFSET_BITS=64 -DFF8_RCHEEVOS -DRC_HASH_NO_ROM -DRC_HASH_NO_ZIP -DRC_HASH_NO_ENCRYPTED ${LH_RA_CFLAGS}
  INCLUDES runtime backends/host ${vendor}/include ${vendor}/src/api ${vendor}/src/client
  LINK_FLAGS -Wl,--wrap=SDL_GetTicks64,--wrap=SDL_CreateThread -pthread
  SANITIZE_FLAGS ${LH_SANITIZE}
  LIBS ${LH_RA_LDFLAGS}
  SOURCES backends/host/ra.c backends/host/ra_http.c backends/host/ra_store.c backends/host/ra_badge.c
          backends/host/player_paths.c backends/host/achievements.c tests/ra_harness.c ${sources})
