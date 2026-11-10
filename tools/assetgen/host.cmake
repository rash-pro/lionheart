# Cross builds must never execute the target compiler's output.
if(CMAKE_CROSSCOMPILING)
  include(ExternalProject)
  find_program(LH_HOST_C_COMPILER NAMES cc clang gcc cl NO_CMAKE_FIND_ROOT_PATH REQUIRED)
  set(assetgen_suffix "")
  if(CMAKE_HOST_WIN32)
    set(assetgen_suffix ".exe")
  endif()
  set(ASSETGEN_COMMAND "${CMAKE_CURRENT_BINARY_DIR}/host-assetgen/assetgen${assetgen_suffix}")
  ExternalProject_Add(lh_host_assetgen
    SOURCE_DIR "${CMAKE_CURRENT_LIST_DIR}"
    BINARY_DIR "${CMAKE_CURRENT_BINARY_DIR}/host-assetgen"
    CMAKE_ARGS "-DCMAKE_C_COMPILER=${LH_HOST_C_COMPILER}" -DCMAKE_BUILD_TYPE=Release
      "-DCMAKE_RUNTIME_OUTPUT_DIRECTORY_RELEASE=${CMAKE_CURRENT_BINARY_DIR}/host-assetgen"
    BUILD_COMMAND "${CMAKE_COMMAND}" --build <BINARY_DIR> --config Release --parallel 4
    BUILD_BYPRODUCTS "${ASSETGEN_COMMAND}"
    BUILD_ALWAYS TRUE INSTALL_COMMAND "")
  set(ASSETGEN_DEPENDENCY lh_host_assetgen)
else()
  add_subdirectory("${CMAKE_CURRENT_LIST_DIR}" "${CMAKE_CURRENT_BINARY_DIR}/host-assetgen")
  set(ASSETGEN_COMMAND "$<TARGET_FILE:assetgen>")
  set(ASSETGEN_DEPENDENCY assetgen)
endif()
