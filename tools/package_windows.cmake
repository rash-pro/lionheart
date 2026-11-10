# Packs the staged Windows player (tools/build_windows.cmake) into
# Lionheart-VERSION-windows-x86_64.zip, one folder named Lionheart.
#   cmake -P tools/package_windows.cmake
# The folder has no version in its name, so a new zip unpacked where the old
# one was replaces the program files and keeps the data a portable folder holds
# beside them. The zip writer fixes order, dates and modes, so the same staged
# files with the same zlib always give the same bytes.
include("${CMAKE_CURRENT_LIST_DIR}/release_common.cmake")
release_default(BUILD "${RELEASE_ROOT}/build/windows")
release_default(OUT "${RELEASE_ROOT}/build/packages")
foreach(path BUILD OUT)
  get_filename_component(${path} "${${path}}" ABSOLUTE)
endforeach()
file(READ "${RELEASE_ROOT}/CMakeLists.txt" top)
if(NOT top MATCHES "project\\(lionheart VERSION ([0-9]+[.][0-9]+[.][0-9]+)")
  message(FATAL_ERROR "No project version in CMakeLists.txt")
endif()
set(VERSION "${CMAKE_MATCH_1}")
set(dist "${BUILD}/dist-windows")
if(NOT EXISTS "${dist}/Lionheart.exe")
  message(FATAL_ERROR "Stage the player first: cmake -P tools/build_windows.cmake")
endif()
set(work "${BUILD}/package")
file(REMOVE_RECURSE "${work}")
file(MAKE_DIRECTORY "${work}")
file(COPY "${dist}/" DESTINATION "${work}/Lionheart")
file(GLOB_RECURSE files LIST_DIRECTORIES FALSE RELATIVE "${work}" "${work}/Lionheart/*")
list(SORT files)
string(JOIN "\n" entries ${files})
file(WRITE "${BUILD}/package-entries.txt" "${entries}\n")
release_run("${CMAKE_COMMAND}" -S "${RELEASE_ROOT}/tools/toolchain-zip" -B "${BUILD}/zip-writer" -DCMAKE_BUILD_TYPE=Release)
release_run("${CMAKE_COMMAND}" --build "${BUILD}/zip-writer" --parallel 2)
file(MAKE_DIRECTORY "${OUT}")
set(package "${OUT}/Lionheart-${VERSION}-windows-x86_64.zip")
execute_process(COMMAND "${BUILD}/zip-writer/toolchain-zip" "${package}" "${BUILD}/package-entries.txt" Lionheart/Lionheart.exe
  WORKING_DIRECTORY "${work}" RESULT_VARIABLE status)
if(NOT status EQUAL 0)
  message(FATAL_ERROR "ZIP writer failed")
endif()
file(SHA256 "${package}" hash)
file(SIZE "${package}" bytes)
message(STATUS "${package}: ${bytes} bytes SHA-256 ${hash}")
