find_package(PkgConfig QUIET)
if(PKG_CONFIG_FOUND)
  pkg_check_modules(LH_GPU_SDL QUIET sdl2)
endif()
if(NOT LH_GPU_SDL_FOUND)
  message(WARNING "SDL2 not found through pkg-config; the test_gpu library will not be built")
  return()
endif()
lh_harness(gpu LIBRARY
  FLAGS -std=c99 -O2 -Wall -Wextra -Wpedantic -Werror ${LH_GPU_SDL_CFLAGS}
  INCLUDES runtime backends/host
  SANITIZE_FLAGS -fsanitize=undefined -fno-sanitize-recover=all
  LIBS ${LH_GPU_SDL_LDFLAGS} z
  SOURCES runtime/gpu.c runtime/gpu_raster_soft.c backends/host/display.c backends/host/screenshot.c
          backends/host/osd.c backends/host/viewport.c tests/gpu_harness.c)
