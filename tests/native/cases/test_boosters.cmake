include(${CMAKE_CURRENT_LIST_DIR}/../support/sdl.cmake)
lh_harness(boosters
  FLAGS -std=c99 -O1 -Wall -Wextra -Wpedantic -Werror -DFF8_SAVESTATES ${LH_SDL_FLAGS}
  INCLUDES runtime backends/host
  RUNTIME_ALL
  SANITIZE_FLAGS ${LH_SANITIZE}
  SOURCES tests/boosters_harness.c backends/host/boosters.c backends/host/audio.c
  LINK_FLAGS ${LH_SDL_LINK})
