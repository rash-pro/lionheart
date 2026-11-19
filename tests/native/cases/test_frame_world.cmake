lh_harness(main FLAGS -std=c99 -O2 -g -Wall -Wextra -Werror INCLUDES runtime
  SOURCES tests/frame_world_harness.c runtime/frame_layer.c runtime/frame_blend.c runtime/gpu.c runtime/gpu_raster_soft.c runtime/gte.c)
