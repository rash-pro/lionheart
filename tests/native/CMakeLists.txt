# Native test suite: a host-side case runner plus the C and C++ harnesses it
# drives. Kept as its own project so that configuring it needs nothing from the
# game build: its add_compile_definitions(FF8_PLAYER) must not reach harnesses.
#
#   cmake -S tests/native -B build/native-tests
#   cmake --build build/native-tests -j6 -- -k
#   ctest --test-dir build/native-tests -j6 --output-on-failure
#
# CMake 3.21 is the first version with ctest --output-junit, which the baseline
# comparison reads. The top-level project is not affected.
cmake_minimum_required(VERSION 3.21)
project(lionheart_native_tests LANGUAGES C CXX)
enable_testing()

get_filename_component(LH_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
file(GLOB LH_RUNTIME_SOURCES CONFIGURE_DEPENDS "${LH_ROOT}/runtime/*.c")
list(SORT LH_RUNTIME_SOURCES)

set(lh_sanitize_default OFF)
if(DEFINED ENV{FF8_TEST_SANITIZE} AND NOT "$ENV{FF8_TEST_SANITIZE}" STREQUAL "")
  set(lh_sanitize_default ON)
endif()
option(FF8_TEST_SANITIZE "Add each harness SANITIZE_FLAGS list, like the variable of that name does for pytest" ${lh_sanitize_default})
set(LH_SANITIZE -fsanitize=address,undefined -fno-omit-frame-pointer)
set(LH_HOST_RUNNER "" CACHE FILEPATH "Prebuilt runner from a native build, required when cross-compiling")

define_property(GLOBAL PROPERTY LH_HARNESSES BRIEF_DOCS "Harness targets" FULL_DOCS "name=target pairs for the runner")

# lh_harness(NAME
#   SOURCES a.c b.cpp        relative to the repository root
#   [FLAGS ...]              C compile flags, copied from the Python cc line
#   [CXX_FLAGS ...]          the same for C++ sources
#   [INCLUDES runtime ...]   relative to the repository root
#   [LINK_FLAGS ...]         only for the link, such as -Wl,--wrap=calloc
#   [SANITIZE_FLAGS ...]     used only with FF8_TEST_SANITIZE, for compile and link
#   [RUNTIME_ALL]            also link every runtime/*.c, built with the same flags
#   [RUNTIME_EXCLUDE hw.c]   names to leave out of RUNTIME_ALL, for a file the fragment replaces
#   [LIBRARY]                build a loadable module for t.library("NAME") instead of a program
#   [LIBS ...])              libraries for the link, after the objects (-lz, -lSDL2)
#
# The harness is known to cases as t.harness("NAME") from the file with the same
# stem as this fragment, so names cannot collide between files.
function(lh_harness name)
  cmake_parse_arguments(PARSE_ARGV 1 H "RUNTIME_ALL;LIBRARY" "" "SOURCES;FLAGS;CXX_FLAGS;INCLUDES;LINK_FLAGS;SANITIZE_FLAGS;LIBS;RUNTIME_EXCLUDE")
  if(H_UNPARSED_ARGUMENTS)
    message(FATAL_ERROR "lh_harness(${name}): unknown arguments ${H_UNPARSED_ARGUMENTS}")
  endif()
  get_filename_component(stem "${CMAKE_CURRENT_LIST_FILE}" NAME_WE)
  set(target "${stem}.${name}")
  # The runner on the build machine cannot load a library built for another system.
  if(H_LIBRARY AND CMAKE_CROSSCOMPILING)
    return()
  endif()

  set(c_flags ${H_FLAGS} -UNDEBUG)
  set(cxx_flags ${H_CXX_FLAGS} -UNDEBUG)
  set(link_flags ${H_LINK_FLAGS})
  if(H_LIBRARY)
    # A loadable module needs position-independent objects, including shared RUNTIME_ALL ones.
    list(APPEND c_flags -fPIC)
    list(APPEND cxx_flags -fPIC)
  endif()
  # A sanitizer named in FLAGS is mandatory for that harness, and compiling with
  # one requires linking with it too, so the guide's flags need no second copy.
  foreach(flag IN LISTS H_FLAGS H_CXX_FLAGS)
    if(flag MATCHES "^-f(no-)?sanitize")
      list(APPEND link_flags "${flag}")
    endif()
  endforeach()
  if(FF8_TEST_SANITIZE)
    list(APPEND c_flags ${H_SANITIZE_FLAGS})
    list(APPEND cxx_flags ${H_SANITIZE_FLAGS})
    list(APPEND link_flags ${H_SANITIZE_FLAGS})
  endif()

  set(includes "")
  foreach(directory IN LISTS H_INCLUDES)
    list(APPEND includes "${LH_ROOT}/${directory}")
  endforeach()

  set(sources "")
  foreach(source IN LISTS H_SOURCES)
    if(IS_ABSOLUTE "${source}")
      list(APPEND sources "${source}")
    else()
      list(APPEND sources "${LH_ROOT}/${source}")
    endif()
  endforeach()

  if(H_LIBRARY)
    add_library("${target}" MODULE ${sources})
    set_target_properties("${target}" PROPERTIES PREFIX "" LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/harness/${stem}")
  else()
    add_executable("${target}" ${sources})
    set_target_properties("${target}" PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/harness/${stem}")
  endif()
  set_target_properties("${target}" PROPERTIES OUTPUT_NAME "${name}")
  target_compile_options("${target}" PRIVATE
    "$<$<COMPILE_LANGUAGE:C>:${c_flags}>" "$<$<COMPILE_LANGUAGE:CXX>:${cxx_flags}>")
  target_include_directories("${target}" PRIVATE ${includes})
  target_link_options("${target}" PRIVATE ${link_flags})
  target_link_libraries("${target}" PRIVATE ${H_LIBS} m)

  if(H_RUNTIME_ALL)
    string(MD5 key "${c_flags};${includes};${H_RUNTIME_EXCLUDE}")
    set(objects "lh_runtime_${key}")
    if(NOT TARGET "${objects}")
      set(runtime_sources ${LH_RUNTIME_SOURCES})
      foreach(excluded IN LISTS H_RUNTIME_EXCLUDE)
        list(FILTER runtime_sources EXCLUDE REGEX "/${excluded}$")
      endforeach()
      add_library("${objects}" OBJECT ${runtime_sources})
      target_compile_options("${objects}" PRIVATE ${c_flags})
      target_include_directories("${objects}" PRIVATE ${includes})
    endif()
    target_sources("${target}" PRIVATE "$<TARGET_OBJECTS:${objects}>")
  endif()

  set_property(GLOBAL APPEND PROPERTY LH_HARNESSES "${target}")
endfunction()

# A function scope per fragment, so a helper variable in one cannot leak into the next.
function(lh_load_fragment file)
  include("${file}")
endfunction()

file(GLOB LH_CASE_FRAGMENTS CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/cases/*.cmake")
foreach(fragment IN LISTS LH_CASE_FRAGMENTS)
  lh_load_fragment("${fragment}")
endforeach()
lh_load_fragment("${CMAKE_CURRENT_SOURCE_DIR}/support/self_tests.cmake")

set(LH_WINDOWS_PATHS 0)
if(CMAKE_SYSTEM_NAME STREQUAL "Windows")
  set(LH_WINDOWS_PATHS 1)
endif()
get_property(LH_HARNESS_TARGETS GLOBAL PROPERTY LH_HARNESSES)
set(LH_BUILD_INFO_TEXT "root=${LH_ROOT}\nemulator=${CMAKE_CROSSCOMPILING_EMULATOR}\nwindows_paths=${LH_WINDOWS_PATHS}\n")
foreach(target IN LISTS LH_HARNESS_TARGETS)
  string(APPEND LH_BUILD_INFO_TEXT "harness.${target}=$<TARGET_FILE:${target}>\n")
endforeach()
set(LH_BUILD_INFO "${CMAKE_BINARY_DIR}/lh_build_info.txt")
file(GENERATE OUTPUT "${LH_BUILD_INFO}" CONTENT "${LH_BUILD_INFO_TEXT}")

set(LH_CASES_FILE "${CMAKE_BINARY_DIR}/lh_cases.cmake")
if(CMAKE_CROSSCOMPILING)
  # Cases run on the build machine and start the cross-built harnesses through
  # the emulator, so this build takes the runner from a native build.
  if(NOT LH_HOST_RUNNER)
    message(FATAL_ERROR "Cross-compiling needs -DLH_HOST_RUNNER=<native build>/lionheart_tests")
  endif()
  execute_process(COMMAND "${LH_HOST_RUNNER}" --build-info "${LH_BUILD_INFO}" --emit-ctest "${LH_CASES_FILE}"
                  RESULT_VARIABLE lh_emit_result)
  if(NOT lh_emit_result EQUAL 0)
    message(FATAL_ERROR "Cannot list cases with ${LH_HOST_RUNNER}")
  endif()
else()
  file(GLOB LH_CASE_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/cases/*.cpp")
  add_executable(lionheart_tests
    support/runner.cpp support/process.cpp support/fixtures.cpp support/self_tests.cpp support/oracle_self_tests.cpp
    ${LH_CASE_SOURCES})
  target_include_directories(lionheart_tests PRIVATE support ${CMAKE_CURRENT_SOURCE_DIR})
  target_compile_features(lionheart_tests PRIVATE cxx_std_17)
  set_target_properties(lionheart_tests PROPERTIES CXX_EXTENSIONS OFF)
  target_compile_options(lionheart_tests PRIVATE -Wall -Wextra -Wpedantic -UNDEBUG)
  target_link_libraries(lionheart_tests PRIVATE ${CMAKE_DL_LIBS})
  # Cases list themselves after each build, so a new case needs no CMake edit.
  add_custom_command(TARGET lionheart_tests POST_BUILD
    COMMAND lionheart_tests --build-info "${LH_BUILD_INFO}" --emit-ctest "${LH_CASES_FILE}"
    VERBATIM)
endif()

set(LH_GUARD "${CMAKE_BINARY_DIR}/lh_cases_guard.cmake")
file(WRITE "${LH_GUARD}"
  "if(EXISTS \"${LH_CASES_FILE}\")\n  include(\"${LH_CASES_FILE}\")\nelse()\n  add_test(lionheart_tests_are_not_built \"${CMAKE_COMMAND}\" -E false)\nendif()\n")
set_property(DIRECTORY APPEND PROPERTY TEST_INCLUDE_FILES "${LH_GUARD}")

if(NOT CMAKE_CROSSCOMPILING)
  add_custom_target(parity
    COMMAND "${CMAKE_COMMAND}" -DCTEST=${CMAKE_CTEST_COMMAND} -DBUILD=${CMAKE_BINARY_DIR} -DINFO=${LH_BUILD_INFO}
            -DRUNNER=$<TARGET_FILE:lionheart_tests> -P "${CMAKE_CURRENT_SOURCE_DIR}/parity.cmake"
    DEPENDS lionheart_tests VERBATIM USES_TERMINAL)
  add_dependencies(parity ${LH_HARNESS_TARGETS})
endif()
