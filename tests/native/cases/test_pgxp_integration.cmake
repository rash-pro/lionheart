set(flags -std=c99 -O2 -Wall -Wextra -Werror)
foreach(variant "exact:" "play:-DFF8_PLAY" "generic:-DFF8_GTE_REFERENCE")
  string(REPLACE ":" ";" variant "${variant}")
  list(GET variant 0 mode)
  list(LENGTH variant parts)
  set(mode_flags "")
  if(parts EQUAL 2)
    list(GET variant 1 mode_flags)
  endif()
  lh_harness(${mode}.off FLAGS ${flags} ${mode_flags} INCLUDES runtime
    SOURCES tests/gte_pgxp_hook_harness.c runtime/gte.c)
  lh_harness(${mode}.on FLAGS ${flags} ${mode_flags} -DFF8_PGXP INCLUDES runtime
    SOURCES tests/gte_pgxp_hook_harness.c runtime/gte.c)
endforeach()
