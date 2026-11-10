include("${CMAKE_CURRENT_LIST_DIR}/fetch_windows_deps.cmake")
release_default(DEPS "${RELEASE_ROOT}/build/windows-deps")
release_default(CACHE "${RELEASE_ROOT}/build/downloads")
release_default(BUILD "${RELEASE_ROOT}/build/windows-tests")
release_default(HOST_BUILD "${RELEASE_ROOT}/build/native-tests")
release_jobs()
fetch_windows_deps("${DEPS}" "${CACHE}")
set(ENV{LLVM_MINGW_ROOT} "${DEP_llvm-mingw}")
set(ENV{WINDOWS_DEPS_ROOT} "${DEPS}/install-ucrt")
release_run("${CMAKE_COMMAND}" -S "${RELEASE_ROOT}/tests/native" -B "${BUILD}"
  "-DCMAKE_TOOLCHAIN_FILE=${RELEASE_ROOT}/cmake/toolchains/windows-llvm-mingw.cmake"
  "-DLH_HOST_RUNNER=${HOST_BUILD}/lionheart_tests" -DCMAKE_CROSSCOMPILING_EMULATOR=wine)
release_run("${CMAKE_COMMAND}" --build "${BUILD}" --parallel "${JOBS}")
