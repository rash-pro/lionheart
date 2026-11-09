include(${CMAKE_CURRENT_LIST_DIR}/../support/presentation.cmake)
set(flags -std=c99 -Wall -Wextra -Werror ${LH_SDL_FLAGS})
lh_harness(osd LIBRARY FLAGS ${flags} INCLUDES runtime backends/host SOURCES backends/host/osd.c LINK_FLAGS ${LH_SDL_LINK})
lh_harness(options FLAGS ${flags} INCLUDES backends/host SOURCES tests/host_options_harness.c backends/host/options.c LINK_FLAGS ${LH_SDL_LINK})
lh_harness(soft FLAGS ${flags} INCLUDES runtime backends/host
  SOURCES tests/osd_soft_host.c backends/host/display.c backends/host/screenshot.c backends/host/osd.c backends/host/viewport.c runtime/gpu.c runtime/gpu_raster_soft.c
  LINK_FLAGS -Wl,--wrap=SDL_CreateWindow,--wrap=SDL_RenderPresent,--wrap=host_osd_frame ${LH_SDL_LINK} -lz)
lh_harness(input FLAGS ${flags} INCLUDES runtime backends/host
  SOURCES tests/osd_input_harness.c backends/host/states_disabled.c backends/host/display.c backends/host/screenshot.c backends/host/osd.c backends/host/viewport.c runtime/gpu.c runtime/gpu_raster_soft.c
  LINK_FLAGS -Wl,--wrap=host_osd_event ${LH_SDL_LINK} -lz)
lh_harness(font FLAGS -std=c99 INCLUDES third_party/stb SOURCES tools/assetgen/assetgen.c)
