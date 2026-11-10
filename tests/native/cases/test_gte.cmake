lh_harness(gte LIBRARY
  FLAGS -std=c99 -O2 -g -Wall -Wextra -Wconversion -Werror -pedantic-errors
  INCLUDES runtime
  SANITIZE_FLAGS -fsanitize=undefined -fno-sanitize-recover=all
  SOURCES runtime/gte.c tests/gte_harness.c)
