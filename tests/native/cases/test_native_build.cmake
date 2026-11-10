# The build service with synthetic sets and a fake compiler: no generator, no
# discs, no real compiler.
if(WIN32)
  set(native_build_libs "")
else()
  set(native_build_libs pthread)
endif()
lh_harness(native_build FLAGS -std=c99 -O1 -Wall -Wextra INCLUDES backends/host
  SOURCES tests/native/cases/native_build_harness.c backends/host/native_build.c LIBS ${native_build_libs})
lh_harness(fake_cc FLAGS -std=c99 -O1 -Wall -Wextra SOURCES tests/native/cases/native_build_fake_cc.c)
