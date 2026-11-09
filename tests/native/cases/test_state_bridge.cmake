set(vendor third_party/rcheevos)
file(GLOB sources CONFIGURE_DEPENDS "${LH_ROOT}/${vendor}/src/runtime/*.c")
list(SORT sources)
foreach(source util/md5.c util/rc_compat.c util/rc_util.c api/rc_api_common.c hash/rc_hash.c hash/rc_hash_disc.c hash/rc_cdreader.c)
  list(APPEND sources ${vendor}/src/${source})
endforeach()
lh_harness(bridge LIBRARY
  FLAGS -std=c99 -O1 -Wall -Wextra -Wpedantic -Werror -D_DEFAULT_SOURCE -D_POSIX_C_SOURCE=200809L
        -D_FILE_OFFSET_BITS=64 -DRC_HASH_NO_ROM -DRC_HASH_NO_ZIP -DRC_HASH_NO_ENCRYPTED
  INCLUDES runtime backends/host ${vendor}/include ${vendor}/src/api
  LINK_FLAGS -pthread -Wl,--wrap=clock_gettime,--wrap=socket,--wrap=setsockopt,--wrap=bind,--wrap=listen,--wrap=getsockname,--wrap=accept,--wrap=send,--wrap=fsync,--wrap=rename
  SANITIZE_FLAGS -fsanitize=undefined -fno-sanitize-recover=all
  SOURCES backends/host/state_bridge.c backends/host/ff8_state.c backends/host/achievements.c
          tests/state_bridge_harness.c tests/state_bridge_syscalls.c ${sources})
lh_harness(bind_failure LIBRARY
  FLAGS -std=c99 -DFF8_BRIDGE_PRELOAD
  SOURCES tests/state_bridge_syscalls.c)
