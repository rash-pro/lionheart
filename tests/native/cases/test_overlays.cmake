lh_harness(probe
  FLAGS -std=gnu99 -O2 -DFF8_STATS
  INCLUDES runtime
  RUNTIME_ALL
  SOURCES tests/overlay_dispatch_harness.c)

lh_harness(profile
  FLAGS -std=gnu99 -O1 -DFF8_STATS
  INCLUDES runtime
  RUNTIME_ALL
  SOURCES tests/native/cases/test_overlays_profile.c)
