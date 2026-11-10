include(${CMAKE_CURRENT_LIST_DIR}/../support/presentation.cmake)
lh_presentation_fixture()
set(common -std=c99 -O1 -g -Wall -Wextra -Werror ${LH_SDL_FLAGS})
set(display backends/host/display.c backends/host/screenshot.c backends/host/osd.c backends/host/viewport.c)
lh_harness(hotkeys FLAGS ${common} INCLUDES runtime backends/host RUNTIME_ALL
  SOURCES tests/hotkeys_harness.c ${display} backends/host/input.c
  LINK_FLAGS -Wl,--wrap=host_osd_event,--wrap=host_osd_post,--wrap=SDL_CreateWindow ${LH_SDL_LINK} -lz
  SANITIZE_FLAGS ${LH_SANITIZE})
lh_harness(soft_capture FLAGS ${common} INCLUDES runtime backends/host
  SOURCES tests/screenshot_host.c ${display} runtime/gpu.c runtime/gpu_raster_soft.c
  LINK_FLAGS -Wl,--wrap=host_osd_event,--wrap=time ${LH_SDL_LINK} -lz)
lh_harness(options FLAGS ${common} INCLUDES runtime backends/host
  SOURCES tests/host_options_harness.c backends/host/options.c LINK_FLAGS ${LH_SDL_LINK} -lz)
lh_harness(directory LIBRARY FLAGS ${common} INCLUDES runtime backends/host
  SOURCES backends/host/screenshot.c backends/host/osd.c LINK_FLAGS ${LH_SDL_LINK} -lz)
lh_harness(queue FLAGS ${common} INCLUDES runtime backends/host
  SOURCES tests/screenshot_queue_harness.c backends/host/screenshot.c backends/host/osd.c
  LINK_FLAGS -Wl,--wrap=host_osd_event ${LH_SDL_LINK} -lz)
