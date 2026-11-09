set(flags -std=c99 -O2 -Wall -Wextra -Werror)
set(sanitize ${LH_SANITIZE} -O1)
lh_harness(exact FLAGS ${flags} -DFF8_SAVESTATES SANITIZE_FLAGS ${sanitize}
  SOURCES tests/spu_noise_clock_harness.c runtime/savestate_io.c)
lh_harness(play FLAGS ${flags} -DFF8_PLAY SANITIZE_FLAGS ${sanitize}
  SOURCES tests/spu_noise_clock_harness.c runtime/savestate_io.c)
