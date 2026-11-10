# Native recompiler generator. Standalone (`cmake -S generator`) or as the
# top-level project's subdirectory; neither path needs Python, SDL or the
# runtime, because the player will embed this library.
cmake_minimum_required(VERSION 3.16)
project(lionheart_generator VERSION 0.1.0 LANGUAGES C)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()
option(LIONHEART_GENERATOR_SANITIZE "Build the generator with address and undefined sanitizers" OFF)
include(fingerprint.cmake)
file(GENERATE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/generator_build.h" CONTENT
  "#define LHG_FINGERPRINT \"${LIONHEART_GENERATOR_FINGERPRINT}\"\n")
if(NOT CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  set(LIONHEART_GENERATOR_FINGERPRINT "${LIONHEART_GENERATOR_FINGERPRINT}" PARENT_SCOPE)
endif()

add_library(lionheart_generator STATIC
  src/base.c src/platform.c src/sha1.c src/sha256.c src/cache.c src/json.c src/disc.c src/lzs.c
  src/decode.c src/discover.c src/catalog.c
  src/effects.c src/fast.c src/precise.c src/locals.c src/emit.c src/emit_modules.c src/generator.c src/session.c)
target_include_directories(lionheart_generator PUBLIC include PRIVATE src)
target_include_directories(lionheart_generator PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")

add_executable(lionheart-gen cli/main.c)
target_link_libraries(lionheart-gen PRIVATE lionheart_generator)
if(WIN32)
  target_link_libraries(lionheart-gen PRIVATE shell32)
endif()

# Reads a synthetic program description; only the differential test uses it.
add_executable(lionheart-gen-spec test/spec_emit.c)
target_link_libraries(lionheart-gen-spec PRIVATE lionheart_generator)
target_include_directories(lionheart-gen-spec PRIVATE src)

foreach(target lionheart_generator lionheart-gen lionheart-gen-spec)
  set_target_properties(${target} PROPERTIES C_STANDARD 99 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
  target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
  if(LIONHEART_GENERATOR_SANITIZE)
    target_compile_options(${target} PRIVATE -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer -g)
    target_link_options(${target} PRIVATE -fsanitize=address,undefined)
  endif()
endforeach()

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  enable_testing()
  add_executable(lionheart-cache-test test/cache.c)
  target_link_libraries(lionheart-cache-test PRIVATE lionheart_generator)
  target_include_directories(lionheart-cache-test PRIVATE src)
  set_target_properties(lionheart-cache-test PROPERTIES C_STANDARD 99 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
  target_compile_options(lionheart-cache-test PRIVATE -Wall -Wextra -Wpedantic)
  if(LIONHEART_GENERATOR_SANITIZE)
    target_compile_options(lionheart-cache-test PRIVATE -fsanitize=address,undefined -fno-sanitize-recover=all)
    target_link_options(lionheart-cache-test PRIVATE -fsanitize=address,undefined)
  endif()
  add_test(NAME generator_cache COMMAND lionheart-cache-test "${CMAKE_CURRENT_BINARY_DIR}/cache-test")
  add_test(NAME setup_cli COMMAND "${CMAKE_COMMAND}"
    "-DSETUP=${CMAKE_CURRENT_SOURCE_DIR}/../tools/setup.cmake" -P "${CMAKE_CURRENT_SOURCE_DIR}/test/setup_cli.cmake")
endif()
