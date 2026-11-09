lh_harness(media
  FLAGS -std=c99 -O1 -g -Wall -Wextra -Wpedantic -Werror
  INCLUDES runtime
  RUNTIME_ALL
  LINK_FLAGS -Wl,--wrap=spu_cd_input
  SANITIZE_FLAGS ${LH_SANITIZE}
  SOURCES tests/media_harness.c)
