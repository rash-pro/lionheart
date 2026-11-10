foreach(fixture spu_decode_memo_harness spu_decode_loop_harness)
  lh_harness(${fixture} FLAGS -std=c99 -O2 -Wall -Wextra -Werror SOURCES tests/${fixture}.c)
endforeach()
