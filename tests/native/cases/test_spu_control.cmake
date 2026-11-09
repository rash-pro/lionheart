foreach(mode exact play)
  set(play)
  if(mode STREQUAL "play")
    set(play -DFF8_PLAY)
  endif()
  lh_harness(${mode} FLAGS -std=c99 -O2 -Wall -Wextra -Werror ${play}
    SANITIZE_FLAGS ${LH_SANITIZE} SOURCES tests/spu_control_harness.c)
endforeach()
