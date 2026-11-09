# Runs the suite and compares the outcome with tests/native/identities.tsv.
#   cmake --build build/native-tests --target parity
# Set STEMS to compare only some files, as a CMake list. Realtime tests are left
# out for the same reason pytest's CI selection leaves them out.
execute_process(COMMAND "${CTEST}" --test-dir "${BUILD}" -j6 -LE realtime --output-junit "${BUILD}/parity.xml")
execute_process(COMMAND "${RUNNER}" --build-info "${INFO}" --compare-results "${BUILD}/parity.xml" ${STEMS}
                RESULT_VARIABLE result)
if(NOT result EQUAL 0)
  message(FATAL_ERROR "native results differ from the baseline")
endif()
