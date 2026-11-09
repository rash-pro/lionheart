include(${CMAKE_CURRENT_LIST_DIR}/../support/sdl.cmake)
set(flags -std=c99 -O1 -g -Wall -Wextra -Wpedantic -Werror -DFF8_STATS -DFF8_SAVESTATES)
lh_harness(state FLAGS ${flags} INCLUDES runtime RUNTIME_ALL SANITIZE_FLAGS ${LH_SANITIZE} SOURCES tests/savestate_harness.c)
set(headers ${CMAKE_CURRENT_BINARY_DIR}/savestate-headers)
file(MAKE_DIRECTORY ${headers})
file(WRITE ${headers}/state_build.h "#define FF8_STATE_BUILD \"0000000000000000000000000000000000000000000000000000000000000000\"\n#define FF8_STATE_PRECISE 1\n")
file(RELATIVE_PATH headers_include ${LH_ROOT} ${headers})
lh_harness(host FLAGS ${flags} ${LH_SDL_FLAGS} INCLUDES runtime backends/host ${headers_include} RUNTIME_ALL SANITIZE_FLAGS ${LH_SANITIZE}
  SOURCES tests/host_states_harness.c backends/host/states.c backends/host/state_disc.c backends/host/disc_iso.c backends/host/input.c
    backends/host/display.c backends/host/screenshot.c backends/host/osd.c backends/host/viewport.c backends/host/audio.c
  LINK_FLAGS -Wl,--wrap=SDL_ClearQueuedAudio,--wrap=SDL_CreateWindow,--wrap=SDL_UpdateTexture,--wrap=host_osd_event ${LH_SDL_LINK} -lz)
lh_harness(disc FLAGS -std=c99 -O2 -Wall -Wextra -Wpedantic -Werror INCLUDES runtime backends/host RUNTIME_ALL SANITIZE_FLAGS ${LH_SANITIZE}
  SOURCES tests/state_disc_harness.c backends/host/state_disc.c backends/host/disc_iso.c LINK_FLAGS -Wl,--wrap=pread)
