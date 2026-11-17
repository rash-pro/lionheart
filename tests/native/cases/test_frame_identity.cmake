lh_harness(main FLAGS -std=c99 -O2 -g -Wall -Wextra -Werror INCLUDES runtime
  SANITIZE_FLAGS -fsanitize=address,undefined -fno-sanitize-recover=all
  SOURCES runtime/frame_identity.c runtime/gpu.c runtime/gpu_raster_soft.c runtime/gte.c tests/frame_identity_harness.c)
