lh_harness(main FLAGS -std=c99 -O2 -Wall -Wextra -Werror -fsanitize=undefined -fno-sanitize-recover=all
  INCLUDES runtime LINK_FLAGS -fsanitize=undefined -fno-sanitize-recover=all
  SOURCES runtime/cpu.c runtime/interp.c tests/precise_bounds_harness.c)
