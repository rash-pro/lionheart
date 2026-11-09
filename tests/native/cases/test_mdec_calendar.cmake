# The span model is compared with the original word calendar through an
# instrumented hw.c, regenerated here so a change to the observation points
# fails at configure time, like the Python's boundary check. Python keeps this
# transform in tools/mdec_trace.py, and check_against_python.py checks they agree.
set(source "${LH_ROOT}/runtime/hw.c")
set(output "${CMAKE_BINARY_DIR}/generated/test_mdec_calendar/hw.c")
file(READ "${source}" hw)
function(lh_insert_after_unique marker addition)
  string(FIND "${hw}" "${marker}" first)
  string(FIND "${hw}" "${marker}" last REVERSE)
  if(first LESS 0 OR NOT first EQUAL last)
    message(FATAL_ERROR "hardware instrumentation boundary changed: ${marker}")
  endif()
  string(REPLACE "${marker}" "${marker}\n${addition}" hw "${hw}")
  set(hw "${hw}" PARENT_SCOPE)
endfunction()
lh_insert_after_unique("#include \"cycle_math.h\"" "void mdec_trace_observe(cpu_t *, unsigned, uint32_t, unsigned, uint32_t);")
lh_insert_after_unique("    if (source >= 11) return;" "    mdec_trace_observe(c, 2, source, 0, 0);")
lh_insert_after_unique("    record(c, address, width, value, 0);" "    mdec_trace_observe(c, 0, address, width, value);")
lh_insert_after_unique("    value &= mask;" "    mdec_trace_observe(c, 1, address, width, value);")
set(previous "")
if(EXISTS "${output}")
  file(READ "${output}" previous)
endif()
if(NOT previous STREQUAL hw)
  file(WRITE "${output}" "${hw}")
endif()
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${source}")

foreach(enabled 0 1)
  lh_harness(model_${enabled} LIBRARY
    FLAGS -std=c99 -O2 -Wall -Wextra -Werror -DFF8_MDEC_CALENDAR=${enabled}
    INCLUDES runtime
    RUNTIME_ALL RUNTIME_EXCLUDE hw.c
    LINK_FLAGS -Wl,-Bsymbolic
    SOURCES tests/mdec_calendar_harness.c "${output}")
endforeach()
