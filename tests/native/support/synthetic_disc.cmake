# Compile a host fixture emitter even when the harnesses target another platform.
get_property(lh_synthetic_configured GLOBAL PROPERTY LH_SYNTHETIC_CONFIGURED)
if(NOT lh_synthetic_configured)
  set_property(GLOBAL PROPERTY LH_SYNTHETIC_CONFIGURED TRUE)
  set(LH_SYNTHETIC_DIR "${CMAKE_BINARY_DIR}/synthetic-disc" CACHE INTERNAL "Synthetic metadata inputs")
  find_program(lh_fixture_cxx NAMES c++ g++ REQUIRED NO_CMAKE_FIND_ROOT_PATH)
  execute_process(COMMAND "${lh_fixture_cxx}" -std=c++17 "${LH_ROOT}/tests/native/support/synthetic_metadata.cpp"
    -o "${CMAKE_BINARY_DIR}/synthetic-metadata" COMMAND_ERROR_IS_FATAL ANY)
  execute_process(COMMAND "${CMAKE_BINARY_DIR}/synthetic-metadata" "${LH_SYNTHETIC_DIR}" COMMAND_ERROR_IS_FATAL ANY)
  file(SHA1 "${LH_SYNTHETIC_DIR}/exe" exe_sha1)
  file(READ "${LH_SYNTHETIC_DIR}/fnv" fnv)
  file(WRITE "${LH_SYNTHETIC_DIR}/exe-identity.json"
    "{\"base\":2147549184,\"size\":2048,\"fnv1a\":\"${fnv}\",\"file_size\":4096,\"sha1\":\"${exe_sha1}\"}")
  set(serials SLUS_008.92 SLUS_009.08 SLUS_009.09 SLUS_009.10)
  foreach(n RANGE 1 4)
    math(EXPR index "${n}-1")
    list(GET serials ${index} serial)
    file(SHA1 "${LH_SYNTHETIC_DIR}/img${n}" img_sha1)
    file(WRITE "${LH_SYNTHETIC_DIR}/files-disc${n}.json"
      "{\"exe\":{\"name\":\"${serial}\",\"sha1\":\"${exe_sha1}\"},\"image\":{\"name\":\"FF8DISC${n}.IMG\",\"lba\":826,\"size\":2048,\"sha1\":\"${img_sha1}\"}}")
  endforeach()
  execute_process(COMMAND "${CMAKE_COMMAND}" "-DINPUT=${LH_SYNTHETIC_DIR}" "-DOUTPUT=${LH_SYNTHETIC_DIR}/disc_identity.h"
    -P "${LH_ROOT}/tools/disc_identity_header.cmake" COMMAND_ERROR_IS_FATAL ANY)
endif()
