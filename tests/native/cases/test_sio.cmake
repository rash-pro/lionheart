include(${CMAKE_CURRENT_LIST_DIR}/../support/sdl.cmake)
set(flags -std=c99 -O1 -g -Wall -Wextra -Werror)
set(display backends/host/display.c backends/host/screenshot.c backends/host/osd.c backends/host/viewport.c)
lh_harness(sio FLAGS ${flags} INCLUDES runtime RUNTIME_ALL SANITIZE_FLAGS ${LH_SANITIZE} SOURCES tests/sio_harness.c)
lh_harness(events FLAGS ${flags} ${LH_SDL_FLAGS} INCLUDES runtime backends/host SANITIZE_FLAGS ${LH_SANITIZE}
  SOURCES tests/sio_host_harness.c ${display} runtime/gpu.c runtime/gpu_raster_soft.c LINK_FLAGS ${LH_SDL_LINK} -lz)
lh_harness(dualshock FLAGS ${flags} ${LH_SDL_FLAGS} INCLUDES runtime backends/host RUNTIME_ALL SANITIZE_FLAGS ${LH_SANITIZE}
  SOURCES tests/host_pad_harness.c ${display} backends/host/input.c
  LINK_FLAGS -Wl,--wrap=SDL_GameControllerRumble,--wrap=SDL_GameControllerSetLED ${LH_SDL_LINK} -lz)
lh_harness(probe FLAGS ${flags} ${LH_SDL_FLAGS} INCLUDES runtime backends/host RUNTIME_ALL SANITIZE_FLAGS ${LH_SANITIZE}
  SOURCES tests/host_pad_probe_harness.c tests/host_pad_probe.c ${display} backends/host/input.c LINK_FLAGS ${LH_SDL_LINK} -lz)
