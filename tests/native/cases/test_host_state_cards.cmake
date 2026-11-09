include(${CMAKE_CURRENT_LIST_DIR}/../support/sdl.cmake)
set(headers ${CMAKE_CURRENT_BINARY_DIR}/host-state-cards)
file(MAKE_DIRECTORY ${headers})
file(WRITE ${headers}/state_build.h "#define FF8_STATE_BUILD \"0000000000000000000000000000000000000000000000000000000000000000\"\n#define FF8_STATE_PRECISE 1\n")
file(RELATIVE_PATH headers_include ${LH_ROOT} ${headers})
lh_harness(cards FLAGS -std=c99 -O1 -g -Wall -Wextra -Wpedantic -Werror -DFF8_STATS -DFF8_SAVESTATES ${LH_SDL_FLAGS}
  INCLUDES runtime backends/host ${headers_include} RUNTIME_ALL SANITIZE_FLAGS ${LH_SANITIZE}
  SOURCES tests/host_state_cards_harness.c backends/host/states.c backends/host/state_disc.c backends/host/disc_iso.c
    backends/host/input.c backends/host/display.c backends/host/screenshot.c backends/host/osd.c backends/host/viewport.c backends/host/audio.c
  LINK_FLAGS -Wl,--wrap=host_osd_event ${LH_SDL_LINK} -lz)
