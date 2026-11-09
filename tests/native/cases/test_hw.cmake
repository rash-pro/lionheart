lh_harness(main FLAGS -std=c99 -O1 -g -Wall -Wextra -Werror -DFF8_STATS INCLUDES runtime RUNTIME_ALL
  SANITIZE_FLAGS ${LH_SANITIZE}
  SOURCES tests/hw_harness.c)
lh_harness(default FLAGS -std=c99 -O2 -Wall -Wextra -Werror INCLUDES runtime RUNTIME_ALL
  SOURCES tests/hw_harness.c)
