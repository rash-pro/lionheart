foreach(mode False True)
  set(play)
  if(mode STREQUAL "True")
    set(play -DFF8_PLAY)
  endif()
  lh_harness(${mode} FLAGS -std=c99 -O2 -Wall -Wextra -Werror ${play}
    SANITIZE_FLAGS ${LH_SANITIZE} SOURCES tests/spu_replay_harness.c)
endforeach()
