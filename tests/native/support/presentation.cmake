include(${CMAKE_CURRENT_LIST_DIR}/sdl.cmake)
function(lh_presentation_fixture)
  lh_harness(viewport LIBRARY FLAGS -std=c99 -Wall -Wextra -Werror INCLUDES runtime SOURCES backends/host/viewport.c)
endfunction()
