lh_harness(main FLAGS -std=c99 -O2 -Wall -Wextra -Werror
  SANITIZE_FLAGS ${LH_SANITIZE}
  SOURCES tests/timer_spans_harness.c)
