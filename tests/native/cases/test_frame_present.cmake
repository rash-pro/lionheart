lh_harness(main FLAGS -std=c99 -O2 -g -Wall -Wextra -Werror -DFF8_SAVESTATES INCLUDES runtime
  SANITIZE_FLAGS -fsanitize=address,undefined -fno-sanitize-recover=all
  SOURCES tests/frame_present_harness.c runtime/frame_present.c runtime/frame_blend.c runtime/frame_layer.c
    runtime/frame_record.c runtime/frame_identity.c runtime/gpu_raster_soft.c runtime/gte.c runtime/savestate_io.c)
lh_harness(scene FLAGS -std=c99 -O2 -g -Wall -Wextra -Werror -DFF8_SAVESTATES INCLUDES runtime
  SOURCES runtime/frame_blend.c runtime/frame_record.c runtime/frame_identity.c runtime/gpu.c runtime/gpu_raster_soft.c
    runtime/gte.c runtime/savestate_io.c tests/frame_blend_harness.c)
lh_harness(inbetween FLAGS -std=c99 -O2 -g -Wall -Wextra -Werror -DFF8_SAVESTATES INCLUDES runtime
  SOURCES tools/devtools/inbetween.c runtime/gpu_raster_soft.c runtime/savestate_io.c runtime/frame_record.c
    runtime/frame_blend.c runtime/frame_layer.c runtime/frame_identity.c runtime/gte.c)
lh_harness(layers FLAGS -std=c99 -O2 -g -Wall -Wextra -Werror -DFF8_SAVESTATES INCLUDES runtime
  SOURCES runtime/frame_layer.c runtime/frame_blend.c runtime/frame_record.c runtime/frame_identity.c runtime/gpu.c
    runtime/gpu_raster_soft.c runtime/gte.c runtime/savestate_io.c tests/frame_layer_harness.c)
include(${CMAKE_CURRENT_LIST_DIR}/../support/presentation.cmake)
lh_presentation_fixture()
lh_harness(host FLAGS -std=c99 -O1 -g -Wall -Wextra -Werror ${LH_SDL_FLAGS} INCLUDES runtime backends/host RUNTIME_ALL
  SOURCES tests/frame_present_host_harness.c backends/host/frame_present_host.c backends/host/frame_identity_host.c
  LINK_FLAGS ${LH_SDL_LINK})
