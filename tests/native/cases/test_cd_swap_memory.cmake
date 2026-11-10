lh_harness(swap FLAGS -std=c99 -O1 -DFF8_STATS INCLUDES runtime RUNTIME_ALL
  LINK_FLAGS -Wl,--wrap=calloc
  SOURCES tests/cd_swap_memory_harness.c)
