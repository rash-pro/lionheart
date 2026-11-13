lh_harness(main FLAGS -std=c99 -O2 -g -Wall -Wextra -Werror -DFF8_SAVESTATES INCLUDES runtime
  SANITIZE_FLAGS -fsanitize=address,undefined -fno-sanitize-recover=all
  SOURCES runtime/frame_blend.c runtime/frame_record.c runtime/frame_identity.c runtime/gpu.c runtime/gpu_raster_soft.c
    runtime/gte.c runtime/savestate_io.c tests/frame_blend_harness.c)
lh_harness(inbetween FLAGS -std=c99 -O2 -g -Wall -Wextra -Werror -DFF8_SAVESTATES INCLUDES runtime
  SOURCES tools/devtools/inbetween.c runtime/gpu_raster_soft.c runtime/savestate_io.c runtime/frame_record.c
    runtime/frame_blend.c runtime/frame_layer.c runtime/frame_identity.c runtime/gte.c)
