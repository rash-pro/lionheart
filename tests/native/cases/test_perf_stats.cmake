foreach(stats IN ITEMS False True)
  set(flags -std=c99 -O2)
  if(stats STREQUAL "True")
    list(APPEND flags -DFF8_STATS)
  endif()
  lh_harness(${stats} LIBRARY FLAGS ${flags} INCLUDES runtime
    SOURCES runtime/cpu.c runtime/interp.c tests/native/cases/test_perf_stats_memory.c)
endforeach()
