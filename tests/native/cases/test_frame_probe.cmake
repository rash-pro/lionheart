foreach(mode False True)
  set(flags -std=c99 -O2 -ffunction-sections)
  if(mode STREQUAL True)
    list(APPEND flags -DSUBSYSTEMS)
  endif()
  lh_harness(${mode} FLAGS ${flags} INCLUDES runtime
    SOURCES tools/frame_probe.c tests/native/cases/test_frame_probe_host.c tests/native/cases/test_frame_probe_main.c
    LINK_FLAGS -Wl,--gc-sections,--wrap=cpu_init,--wrap=host_present,--wrap=hw_destroy)
endforeach()
# A frozen host link receipt lets the native driver exercise the same relink path
# as a real Makefiles build without requiring owned game inputs.
add_library(frame_probe_frozen_objects OBJECT
  "${LH_ROOT}/tests/native/cases/test_frame_probe_host.c"
  "${LH_ROOT}/tests/native/cases/test_frame_probe_main.c"
  "${LH_ROOT}/tests/native/cases/test_frame_probe_stubs.c")
target_include_directories(frame_probe_frozen_objects PRIVATE "${LH_ROOT}/runtime")
target_compile_options(frame_probe_frozen_objects PRIVATE -O2 -ffunction-sections)
file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/frame-probe-frozen/CMakeFiles/ff8.dir")
file(WRITE "${CMAKE_BINARY_DIR}/frame-probe-frozen/CMakeCache.txt" "FF8_STATS:BOOL=OFF\n")
file(GENERATE OUTPUT "${CMAKE_BINARY_DIR}/frame-probe-frozen/CMakeFiles/ff8.dir/link.txt"
  CONTENT "${CMAKE_C_COMPILER} -Wl,--gc-sections $<JOIN:$<TARGET_OBJECTS:frame_probe_frozen_objects>, > -o ff8\n")
lh_harness(driver CXX_FLAGS -std=c++17 "-DLH_TOOLS_ROOT=\"${LH_ROOT}\""
  SOURCES tools/devtools/frame_probe.cpp)
add_dependencies(test_frame_probe.driver frame_probe_frozen_objects)
