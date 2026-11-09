lh_harness(main FLAGS -std=c99 -O1 -g -Wall -Wextra -Wpedantic -Werror INCLUDES runtime RUNTIME_ALL
  SANITIZE_FLAGS ${LH_SANITIZE}
  SOURCES tests/bios_card_harness.c)
