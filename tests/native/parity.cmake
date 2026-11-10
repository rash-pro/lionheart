# Runs the suite and compares the outcome with tests/native/identities.tsv.
#   cmake --build build/native-tests --target parity
# Set STEMS to compare selected files. Realtime cases are excluded.
# CTEST_PARALLEL_LEVEL controls jobs; otherwise use the machine's core count.
if(DEFINED ENV{CTEST_PARALLEL_LEVEL} AND NOT "$ENV{CTEST_PARALLEL_LEVEL}" STREQUAL "")
  set(jobs "$ENV{CTEST_PARALLEL_LEVEL}")
else()
  cmake_host_system_information(RESULT jobs QUERY NUMBER_OF_LOGICAL_CORES)
endif()
if(DEFINED ENV{LH_PARITY_STEMS})
  set(STEMS "$ENV{LH_PARITY_STEMS}")
endif()
set(selection "")
if(STEMS)
  list(JOIN STEMS "|" stems_regex)
  set(selection -R "^(${stems_regex})::")
endif()
execute_process(COMMAND "${CTEST}" --test-dir "${BUILD}" -j${jobs} -LE realtime ${selection}
                --output-junit "${BUILD}/parity.xml")
execute_process(COMMAND "${RUNNER}" --build-info "${INFO}" --compare-results "${BUILD}/parity.xml" ${STEMS}
                RESULT_VARIABLE result)
if(NOT result EQUAL 0)
  message(FATAL_ERROR "native results differ from the baseline")
endif()
