include(${CMAKE_CURRENT_LIST_DIR}/../support/sdl.cmake)
lh_harness(options FLAGS -std=c99 -Wall -Wextra -Werror ${LH_SDL_FLAGS} INCLUDES backends/host
  SOURCES tests/host_options_harness.c backends/host/options.c LINK_FLAGS ${LH_SDL_LINK})
