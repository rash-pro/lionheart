include("${CMAKE_CURRENT_LIST_DIR}/release_common.cmake")

function(player_licences dist)
  set(dest "${dist}/licences")
  file(MAKE_DIRECTORY "${dest}")
  set(notices
    third_party/parallel-psx/COPYING parallel-psx-ISC.txt
    third_party/rcheevos/LICENSE rcheevos-MIT.txt
    third_party/spleen/LICENSE Spleen-BSD-2.txt
    third_party/imgui/LICENSE.txt Dear-ImGui-MIT.txt
    third_party/nativefiledialog-extended/LICENSE nativefiledialog-zlib.txt
    third_party/chakra-petch/OFL.txt Chakra-Petch-OFL.txt
    third_party/xz-embedded/COPYING xz-embedded-0BSD.txt
    LICENSE Lionheart-MIT.txt
    THIRD_PARTY_NOTICES.md THIRD_PARTY_NOTICES.md
    licenses/curl.txt curl.txt
    third_party/parallel-psx/volk/volk.h volk-MIT.h)
  while(notices)
    list(POP_FRONT notices source name)
    configure_file("${RELEASE_ROOT}/${source}" "${dest}/${name}" COPYONLY)
  endwhile()
  if(PLATFORM STREQUAL "windows")
    configure_file("${DEP_SDL2}/LICENSE.txt" "${dest}/SDL2-zlib.txt" COPYONLY)
    configure_file("${DEP_ffmpeg}/COPYING.LGPLv2.1" "${dest}/FFmpeg-LGPL-2.1.txt" COPYONLY)
    configure_file("${DEP_zlib}/LICENSE" "${dest}/zlib.txt" COPYONLY)
    file(READ "${PREFIX}/ffmpeg-configure.txt" configure)
    string(STRIP "${configure}" configure)
    if(configure STREQUAL "")
      message(FATAL_ERROR "Missing FFmpeg configure provenance")
    endif()
    string(REGEX REPLACE "^[^ \t\r\n]+" "$FFMPEG_SOURCE/configure" configure "${configure}")
    string(REGEX REPLACE "--prefix=[^ \t\r\n]+" "--prefix=\${PREFIX}" configure "${configure}")
    string(REGEX REPLACE "[ \t\r\n]+" " " configure "${configure}")
    string(STRIP "${configure}" configure)
    file(WRITE "${dest}/FFmpeg-configure.txt" "${configure}\n")
    configure_file("${DEP_llvm-mingw}/LICENSE.TXT" "${dest}/llvm-mingw-runtime.txt" COPYONLY)
    foreach(name COPYING.MinGW-w64-runtime.txt COPYING.winpthreads.txt)
      configure_file("${DEP_llvm-mingw}/x86_64-w64-mingw32/share/mingw32/${name}" "${dest}/${name}" COPYONLY)
    endforeach()
    # Preserve the distribution notice bytes while both release recipes coexist.
    file(WRITE "${dest}/FFmpeg-source.txt"
      "FFmpeg 7.1.1, unmodified source: https://ffmpeg.org/releases/ffmpeg-7.1.1.tar.xz\n"
      "LGPL shared libraries; no GPL, nonfree or version3 enabled.\n"
      "Rebuild with tools/build_windows.cmake; exact configure line is adjacent.\n"
      "You may replace the DLLs with compatible modified versions. No static FFmpeg linkage.\n"
      "Distributors must preserve LGPL notices and make corresponding source available;\n"
      "this source URL and the checked-in pinned fetch/build recipe accompany this distribution.\n")
  endif()
endfunction()

function(stage_player)
  release_default(PLATFORM linux)
  release_default(DIST "${BUILD}/dist-${PLATFORM}")
  file(MAKE_DIRECTORY "${DIST}")
  if(PLATFORM STREQUAL "linux")
    configure_file("${BUILD}/lionheart" "${DIST}/lionheart" COPYONLY)
  elseif(PLATFORM STREQUAL "windows")
    configure_file("${BUILD}/Lionheart.exe" "${DIST}/Lionheart.exe" COPYONLY)
    foreach(dll SDL2.dll avformat-61.dll avcodec-61.dll avutil-59.dll swscale-8.dll)
      configure_file("${PREFIX}/bin/${dll}" "${DIST}/${dll}" COPYONLY)
    endforeach()
    foreach(dll libc++.dll libunwind.dll libwinpthread-1.dll)
      set(source "${DEP_llvm-mingw}/x86_64-w64-mingw32/bin/${dll}")
      if(EXISTS "${source}")
        configure_file("${source}" "${DIST}/${dll}" COPYONLY)
      endif()
    endforeach()
    configure_file("${RELEASE_ROOT}/tools/windows-deps.json" "${DIST}/dependencies.json" COPYONLY)
  else()
    message(FATAL_ERROR "PLATFORM must be linux or windows")
  endif()
  player_licences("${DIST}")
  message(STATUS "Distribution: ${DIST}")
endfunction()

if(CMAKE_SCRIPT_MODE_FILE STREQUAL CMAKE_CURRENT_LIST_FILE)
  release_default(PLATFORM linux)
  release_default(BUILD "${RELEASE_ROOT}/build/${PLATFORM}")
  release_jobs()
  if(PLATFORM STREQUAL "windows")
    include("${CMAKE_CURRENT_LIST_DIR}/fetch_windows_deps.cmake")
    release_default(DEPS "${RELEASE_ROOT}/build/windows-deps")
    release_default(CACHE "${RELEASE_ROOT}/build/downloads")
    fetch_windows_deps("${DEPS}" "${CACHE}")
    set(PREFIX "${DEPS}/install-ucrt")
  elseif(NOT STAGE_ONLY)
    release_run("${CMAKE_COMMAND}" -S "${RELEASE_ROOT}" -B "${BUILD}" -DFF8_PLAYER=ON -DCMAKE_BUILD_TYPE=Release)
    release_run("${CMAKE_COMMAND}" --build "${BUILD}" --target ff8 --parallel "${JOBS}")
  endif()
  stage_player()
endif()
