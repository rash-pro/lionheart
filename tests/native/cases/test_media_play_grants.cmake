set(flags -std=c99 -O2 -Wall -Wextra -Werror)
lh_harness(play FLAGS ${flags} -DFF8_PLAY INCLUDES runtime RUNTIME_ALL
  LINK_FLAGS -Wl,--wrap=spu_cd_input SOURCES tests/media_harness.c)
lh_harness(alias_exact LIBRARY FLAGS ${flags} INCLUDES runtime RUNTIME_ALL
  LINK_FLAGS -Wl,-Bsymbolic SOURCES tests/mdec_calendar_harness.c)
lh_harness(alias_play LIBRARY FLAGS ${flags} -DFF8_PLAY INCLUDES runtime RUNTIME_ALL
  LINK_FLAGS -Wl,-Bsymbolic SOURCES tests/mdec_calendar_harness.c)
