# First run of the player with synthetic discs and a fake compiler archive the
# case serves itself on the loopback address: no network, no game data.
include(${CMAKE_CURRENT_LIST_DIR}/../support/synthetic_disc.cmake)
include(${CMAKE_CURRENT_LIST_DIR}/../support/network.cmake)
add_compile_definitions(LH_SYNTHETIC_METADATA="${LH_SYNTHETIC_DIR}")
if(WIN32)
  # As in test_host_gui: the GUI harness links partial ELF objects, and the
  # synthetic player is built on demand for Linux only.
  return()
endif()
lh_harness(fake_zig FLAGS -std=c99 -O1 -Wall -Wextra SOURCES tests/native/cases/native_setup_fake_zig.c)
include(${CMAKE_CURRENT_LIST_DIR}/../support/sdl.cmake)
lh_harness(gui FLAGS -std=c99 -O1 -Wall -Wextra -Wpedantic -Werror -DFF8_PLAYER -DFF8_GUI
  -ffunction-sections -fdata-sections ${LH_SDL_FLAGS} INCLUDES runtime backends/host
  SOURCES tests/gui_harness.c backends/host/gui.c backends/host/gui_ini.c backends/host/gui_media.c
  backends/host/gui_browser.c backends/host/options.c backends/host/player_paths.c backends/host/disc_id.c
  backends/host/disc_iso.c runtime/memcard.c runtime/cpu.c LINK_FLAGS -Wl,--gc-sections ${LH_SDL_LINK})
target_include_directories(test_native_setup.gui PRIVATE "${LH_SYNTHETIC_DIR}")
