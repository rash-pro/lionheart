lh_harness(sha1
  FLAGS -std=c99 -O2 -Wall -Wextra -Werror -ffunction-sections -fdata-sections
  INCLUDES runtime
  LINK_FLAGS -Wl,--gc-sections
  SOURCES tests/native/cases/test_code_sha1.c runtime/cpu.c)
