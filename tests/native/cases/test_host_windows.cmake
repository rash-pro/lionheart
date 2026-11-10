file(STRINGS "${LH_ROOT}/CMakeLists.txt" version_line REGEX "^project\\(lionheart VERSION")
string(REGEX MATCH "VERSION ([0-9.]+)" version_match "${version_line}")
set(PROJECT_VERSION "${CMAKE_MATCH_1}")
include("${LH_ROOT}/cmake/player_identity.cmake")
include(${CMAKE_CURRENT_LIST_DIR}/../support/sdl.cmake)
lh_harness(session FLAGS -std=c99 -DFF8_PLAYER -Wall -Wextra -Werror "-I${CMAKE_CURRENT_BINARY_DIR}" ${LH_SDL_FLAGS}
  INCLUDES backends/host SOURCES backends/host/player_session.c backends/host/player_paths.c tests/native/cases/test_host_windows_session.c
  LINK_FLAGS -Wl,--wrap=SDL_ShowSimpleMessageBox ${LH_SDL_LINK})
lh_harness(sha FLAGS -std=c99 -Wall -Wextra -Werror INCLUDES backends/host SOURCES tests/native/cases/test_host_windows_sha.c)
