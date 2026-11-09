# Runs the suite and compares the outcome with tests/native/identities.tsv.
#   cmake --build build/native-tests --target parity
# Set STEMS to compare only some files, as a CMake list. Realtime tests are left
# out for the same reason pytest's CI selection leaves them out. The job count is
# CTEST_PARALLEL_LEVEL from the environment, defaulting to the machine's core count.
if(DEFINED ENV{CTEST_PARALLEL_LEVEL} AND NOT "$ENV{CTEST_PARALLEL_LEVEL}" STREQUAL "")
  set(jobs "$ENV{CTEST_PARALLEL_LEVEL}")
else()
  cmake_host_system_information(RESULT jobs QUERY NUMBER_OF_LOGICAL_CORES)
endif()
execute_process(COMMAND "${CTEST}" --test-dir "${BUILD}" -j${jobs} -LE realtime --output-junit "${BUILD}/parity.xml")
execute_process(COMMAND "${RUNNER}" --build-info "${INFO}" --compare-results "${BUILD}/parity.xml" ${STEMS}
                RESULT_VARIABLE result)
if(NOT result EQUAL 0)
  message(FATAL_ERROR "native results differ from the baseline")
endif()
