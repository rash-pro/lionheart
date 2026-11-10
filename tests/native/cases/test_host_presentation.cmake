include(${CMAKE_CURRENT_LIST_DIR}/../support/sdl.cmake)
lh_harness(presentation FLAGS -std=c99 -O1 -g -Wall -Wextra -Werror ${LH_SDL_FLAGS}
  INCLUDES runtime backends/host
  SANITIZE_FLAGS ${LH_SANITIZE}
  SOURCES tests/host_presentation_harness.c backends/host/display.c backends/host/screenshot.c
    backends/host/osd.c backends/host/viewport.c runtime/gpu.c runtime/gpu_raster_soft.c
  LINK_FLAGS -Wl,--wrap=SDL_SetTextureScaleMode,--wrap=SDL_CreateWindow,--wrap=SDL_SetWindowFullscreen,--wrap=SDL_RenderCopy ${LH_SDL_LINK} -lz)
