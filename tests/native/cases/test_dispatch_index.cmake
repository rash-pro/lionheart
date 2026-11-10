foreach(mode entry precise)
  if(mode STREQUAL "entry")
    set(precise 0)
  else()
    set(precise 1)
  endif()
  lh_harness(${mode}
    FLAGS -std=c99 -O2 -Wall -Wextra -Werror -fsanitize=undefined -fno-sanitize-recover=all -DTEST_PRECISE=${precise}
    INCLUDES runtime
    SOURCES runtime/cpu.c runtime/interp.c tests/dispatch_index_harness.c)
endforeach()
