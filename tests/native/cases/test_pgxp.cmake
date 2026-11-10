lh_harness(cache FLAGS -std=c99 -O2 -Wall -Wextra -Wpedantic -Werror -fsanitize=undefined -fno-sanitize-recover=all -DFF8_PGXP
  INCLUDES runtime SOURCES tests/pgxp_harness.c runtime/pgxp.c runtime/gpu.c runtime/gpu_raster_soft.c LINK_FLAGS -Wl,--wrap=gpu_raster_triangle)
lh_harness(record FLAGS -std=c99 -O2 -Wall -Wextra -Wpedantic -Werror -DFF8_PGXP INCLUDES runtime backends/host
  SOURCES tests/pgxp_record_harness.c runtime/pgxp.c runtime/gpu_raster_soft.c backends/host/gpu_record.c
  LINK_FLAGS -Wl,--wrap=gpu_gp0 -Wl,--wrap=gpu_gp0_words -Wl,--wrap=gpu_gp1 -Wl,--wrap=gpu_vblank -Wl,--wrap=gpu_read -Wl,--wrap=gpu_status -Wl,--wrap=pgxp_bind)
lh_harness(quality CXX_FLAGS -std=c++17 SOURCES tools/devtools/render_quality.cpp)
