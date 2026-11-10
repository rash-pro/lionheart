# Builds the recompiled modules of a generated tree with the system compiler.
# Developers use this to exercise module loading without the player's own build
# orchestration, which compiles the same sources on the user's machine.
#
#   cmake -S tools/native-modules -B build/modules -DGENERATED_DIR=build/generated-mod
#   cmake --build build/modules -j8
cmake_minimum_required(VERSION 3.21)
project(lionheart_native_modules LANGUAGES C)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()
get_filename_component(LH_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
set(GENERATED_DIR "" CACHE PATH "Generated tree holding modules.cmake")
option(FF8_STATS "Match a host built with statistics" OFF)
set(LH_ONLY "" CACHE STRING "Semicolon-separated module names; empty builds every module")
if(NOT EXISTS "${GENERATED_DIR}/modules.cmake")
  message(FATAL_ERROR "Set GENERATED_DIR to a tree generated with --shape modules")
endif()
include("${GENERATED_DIR}/modules.cmake")
include("${LH_ROOT}/cmake/native_module_abi.cmake")
lh_native_module_abi_id(LH_MODULE_ABI_ID "${LH_ROOT}" "${FF8_STATS}")

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
foreach(module IN LISTS LH_MODULES)
  if(LH_ONLY AND NOT module IN_LIST LH_ONLY)
    continue()
  endif()
  set(target "lh_module_${module}")
  string(REPLACE "-" "_" target "${target}")
  add_library("${target}" MODULE ${LH_MODULE_SOURCES_${module}})
  set_target_properties("${target}" PROPERTIES
    PREFIX "" OUTPUT_NAME "${module}" C_VISIBILITY_PRESET hidden
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/modules")
  target_include_directories("${target}" PRIVATE "${LH_ROOT}/runtime")
  target_compile_definitions("${target}" PRIVATE FF8_NATIVE_MODULE "LH_MODULE_ABI_ID=\"${LH_MODULE_ABI_ID}\"")
  if(FF8_STATS)
    target_compile_definitions("${target}" PRIVATE FF8_STATS)
  endif()
  # Anything but the call table is a mistake; fail here rather than at load.
  if(NOT WIN32)
    target_link_options("${target}" PRIVATE -Wl,--no-undefined)
  endif()
endforeach()
