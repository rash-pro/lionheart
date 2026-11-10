# Starts a packaged player headless with no discs and an empty data folder, and
# fails unless it stops where a first start without discs must: asking for
# disc 1, exit code 2.
#   cmake -DPLAYER=Lionheart/Lionheart.exe [-DEMULATOR=wine] -P tools/smoke_package.cmake
cmake_minimum_required(VERSION 3.21)
if(NOT DEFINED PLAYER OR NOT EXISTS "${PLAYER}")
  message(FATAL_ERROR "Specify -DPLAYER=path to the packaged player")
endif()
get_filename_component(PLAYER "${PLAYER}" ABSOLUTE)
get_filename_component(folder "${PLAYER}" DIRECTORY)
if(NOT DEFINED WORK)
  set(WORK "${folder}/../smoke")
endif()
get_filename_component(WORK "${WORK}" ABSOLUTE)
file(REMOVE_RECURSE "${WORK}")
file(MAKE_DIRECTORY "${WORK}/data" "${WORK}/home")
file(WRITE "${WORK}/empty.ini" "")
# Wherever the player keeps its data, it starts empty.
set(ENV{APPDATA} "${WORK}/data")
set(ENV{XDG_DATA_HOME} "${WORK}/data")
set(ENV{XDG_CONFIG_HOME} "${WORK}/data")
set(ENV{HOME} "${WORK}/home")
set(ENV{SDL_VIDEODRIVER} dummy)
set(ENV{SDL_AUDIODRIVER} dummy)
if(DEFINED EMULATOR)
  separate_arguments(launcher NATIVE_COMMAND "${EMULATOR}")
endif()
execute_process(COMMAND ${launcher} "${PLAYER}" --headless --config "${WORK}/empty.ini"
  WORKING_DIRECTORY "${WORK}" RESULT_VARIABLE status OUTPUT_VARIABLE out ERROR_VARIABLE err TIMEOUT 120)
if(NOT status EQUAL 2 OR NOT err MATCHES "Provide FF8 USA disc 1 with --disc PATH")
  message(FATAL_ERROR "Expected the missing-disc exit (2), got ${status}:\n${out}${err}")
endif()
message(STATUS "${PLAYER}: stops at the missing disc")
