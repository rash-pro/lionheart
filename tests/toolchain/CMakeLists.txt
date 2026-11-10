cmake_minimum_required(VERSION 3.21)
project(toolchain_contract LANGUAGES C)
set(CMAKE_C_STANDARD 99)
find_package(ZLIB REQUIRED)
include(../../cmake/toolchain.cmake)
add_executable(toolchain_contract ../toolchain_harness.c ../../generator/src/sha256.c)
target_include_directories(toolchain_contract PRIVATE ../../generator/src)
target_link_libraries(toolchain_contract PRIVATE lionheart_toolchain)
# This entry point keeps platform contracts independent of unrelated harness
# dependencies, while using the same native case runner and loopback fixture.
set(LH_HOST_RUNNER "" CACHE FILEPATH "Native case runner")
if(LH_HOST_RUNNER)
  enable_testing()
  if(WIN32)
    set(windows_paths 1)
  else()
    set(windows_paths 0)
  endif()
  get_filename_component(root ../.. ABSOLUTE BASE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
  file(GENERATE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/build-info.txt" CONTENT
    "root=${root}\nemulator=${CMAKE_CROSSCOMPILING_EMULATOR}\nwindows_paths=${windows_paths}\nharness.test_toolchain.install=$<TARGET_FILE:toolchain_contract>\n")
  foreach(case offline_and_receipt reject_archive_and_cancel http_resume_fallback_and_no_requests concurrent_installers
      no_download_address_preserves_offline_install tar_xz_installs_only_the_listed_files
      tar_xz_names_from_long_name_entries_pax_headers_and_ustar_prefixes tar_xz_refuses_entries_that_leave_the_destination
      tar_xz_refuses_damaged_and_incomplete_archives tar_xz_cancelled_while_unpacking_installs_on_retry)
    add_test(NAME ${case} COMMAND "${LH_HOST_RUNNER}" --build-info
      "${CMAKE_CURRENT_BINARY_DIR}/build-info.txt" --run "test_toolchain::${case}")
  endforeach()
endif()
