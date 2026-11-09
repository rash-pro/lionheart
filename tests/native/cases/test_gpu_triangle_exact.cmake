lh_harness(main FLAGS -std=c99 -O2 -Wall -Wextra -Werror -fsanitize=undefined -fno-sanitize-recover=all INCLUDES runtime
  LINK_FLAGS -fsanitize=undefined -fno-sanitize-recover=all
  SOURCES tests/gpu_triangle_harness.c)
