include(${CMAKE_CURRENT_LIST_DIR}/../support/synthetic_disc.cmake)
include(${CMAKE_CURRENT_LIST_DIR}/../support/sdl.cmake)
lh_harness(gui FLAGS -std=c99 -O1 -Wall -Wextra -Wpedantic -Werror -DFF8_PLAYER -DFF8_GUI
  -ffunction-sections -fdata-sections ${LH_SDL_FLAGS} INCLUDES runtime backends/host
  SOURCES tests/gui_harness.c backends/host/gui.c backends/host/gui_ini.c backends/host/gui_media.c
  backends/host/gui_browser.c backends/host/options.c backends/host/player_paths.c backends/host/disc_id.c
  backends/host/disc_iso.c runtime/memcard.c runtime/cpu.c LINK_FLAGS -Wl,--gc-sections ${LH_SDL_LINK})
target_include_directories(test_host_gui.gui PRIVATE "${LH_SYNTHETIC_DIR}")
lh_harness(script FLAGS -std=c99 -Wall -Wextra -Wpedantic -Werror ${LH_SDL_FLAGS} INCLUDES backends/host
  SOURCES tests/gui_script_harness.c backends/host/gui_test.c LINK_FLAGS ${LH_SDL_LINK})
lh_harness(audio FLAGS -std=c99 -O1 -Wall -Wextra -Werror ${LH_SDL_FLAGS} INCLUDES runtime
  SOURCES tests/gui_audio_harness.c runtime/spu.c LINK_FLAGS ${LH_SDL_LINK})
lh_harness(input FLAGS -std=c99 -O1 -g -Wall -Wextra -Werror -DFF8_GUI ${LH_SDL_FLAGS}
  INCLUDES runtime backends/host RUNTIME_ALL SANITIZE_FLAGS ${LH_SANITIZE}
  SOURCES tests/gui_input_harness.c backends/host/display.c backends/host/input.c
  backends/host/screenshot.c backends/host/osd.c backends/host/viewport.c LINK_FLAGS ${LH_SDL_LINK} -lz)
include(${LH_ROOT}/tools/assetgen/host.cmake)
# The asset converter runs on the build host, including for cross-built harnesses.
set(assets "${CMAKE_BINARY_DIR}/gui-assets")
file(MAKE_DIRECTORY "${assets}")
add_custom_command(OUTPUT "${assets}/gui_assets.h" COMMAND ${ASSETGEN_COMMAND} gui "${LH_ROOT}" "${assets}/gui_assets.h"
  DEPENDS ${ASSETGEN_DEPENDENCY} "${LH_ROOT}/third_party/chakra-petch/ChakraPetch-Regular.ttf"
    "${LH_ROOT}/third_party/chakra-petch/ChakraPetch-SemiBold.ttf"
    "${LH_ROOT}/assets/branding/icon-128.png" VERBATIM)
foreach(name held-entry navigation)
  if(name STREQUAL held-entry)
    set(harness tests/gui_held_entry_harness.cpp)
  else()
    set(harness tests/gui_nav_harness.cpp)
  endif()
  lh_harness(${name} CXX_FLAGS -std=c++17 -O0 -Wall -Wextra -Werror ${LH_SDL_FLAGS}
    INCLUDES backends/host third_party/imgui SOURCES ${harness} backends/host/gui_imgui.cpp
    third_party/imgui/imgui.cpp third_party/imgui/imgui_draw.cpp third_party/imgui/imgui_widgets.cpp
    third_party/imgui/imgui_tables.cpp third_party/imgui/backends/imgui_impl_sdl2.cpp
    third_party/imgui/backends/imgui_impl_sdlrenderer2.cpp "${assets}/gui_assets.h" LINK_FLAGS ${LH_SDL_LINK})
  target_include_directories(test_host_gui.${name} PRIVATE "${assets}")
endforeach()
