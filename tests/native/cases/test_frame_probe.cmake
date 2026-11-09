foreach(mode False True)
  set(flags -std=c99 -O2 -ffunction-sections)
  if(mode STREQUAL True)
    list(APPEND flags -DSUBSYSTEMS)
  endif()
  lh_harness(${mode} FLAGS ${flags} INCLUDES runtime
    SOURCES tools/frame_probe.c tests/native/cases/test_frame_probe_host.c tests/native/cases/test_frame_probe_main.c
    LINK_FLAGS -Wl,--gc-sections,--wrap=cpu_init,--wrap=host_present,--wrap=hw_destroy)
endforeach()
