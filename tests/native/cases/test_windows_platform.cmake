if(WIN32)
  include(${CMAKE_CURRENT_LIST_DIR}/../support/sdl.cmake)
  lh_harness(http LIBRARY FLAGS -std=c99 ${LH_SDL_FLAGS}
    INCLUDES backends/host SOURCES backends/host/ra_http.c tests/native/cases/test_windows_http.c
    LIBS ${LH_SDL_LINK} winhttp)
  lh_harness(io LIBRARY FLAGS -std=c99 -Wall -Wextra -Werror
    INCLUDES backends/host SOURCES backends/host/windows_io.c tests/native/cases/test_windows_io.c)
endif()
