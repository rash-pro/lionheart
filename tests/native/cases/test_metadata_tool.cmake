if(WIN32)
  # The metadata utility starts POSIX processes; it is a build-host tool.
  return()
endif()
lh_harness(ff8files SOURCES tools/devtools/ff8files.cpp tools/devtools/metadata_io.c
  generator/src/base.c generator/src/platform.c generator/src/disc.c
  generator/src/lzs.c generator/src/sha1.c
  INCLUDES generator/src generator/include CXX_FLAGS -std=c++17)
target_compile_definitions(test_metadata_tool.ff8files PRIVATE LH_TOOLS_ROOT="${LH_ROOT}")
