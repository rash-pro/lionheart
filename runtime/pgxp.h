#ifndef FF8_PGXP_H
#define FF8_PGXP_H
#include "gpu_raster.h"
#ifdef __cplusplus
extern "C" {
#endif
struct cpu;
struct gpu;
enum { PGXP_GENERATIONS = 5 };
typedef enum { PGXP_OFF, PGXP_VERTEX, PGXP_PERSPECTIVE } pgxp_mode_t;
typedef enum { PGXP_STRICT, PGXP_LATEST } pgxp_policy_t;
typedef struct { float x, y, w; } pgxp_vertex_t;
typedef struct {
    uint64_t projections, polygons, candidates, precise, ambiguous, misses;
    uint64_t generation_misses, collisions, rejected, overflow;
    uint64_t precise_age[PGXP_GENERATIONS];
    uint64_t partial, vertex_lookups, vertex_precise, vertex_ambiguous;
} pgxp_stats_t;
typedef void (*pgxp_sink_t)(void *, const struct cpu *, uint32_t, int64_t, int64_t, uint32_t);

typedef void (*pgxp_mark_sink_t)(void *, uint32_t);
typedef void (*pgxp_resolved_sink_t)(void *, uint64_t, uint32_t, pgxp_vertex_t, unsigned, unsigned, unsigned);

/* One host GPU per process. No cpu_t, GPU parser, or guest memory fields added. */
void pgxp_bind(struct gpu *, pgxp_mode_t);
void pgxp_record(const struct cpu *, uint32_t packed_sxy, int64_t x_q16, int64_t y_q16, uint32_t z);
void pgxp_vblank(struct gpu *);
void pgxp_reset(struct gpu *);
void pgxp_polygon(struct gpu *, const gpu_vertex_t *, unsigned count, int offset_x, int offset_y);
void pgxp_select(struct gpu *, unsigned first);
int pgxp_triangle(const gpu_raster_t *, pgxp_vertex_t out[3]);
pgxp_stats_t pgxp_stats(void);
/* Export oldest to newest, inserting generation boundaries between visits. */
void pgxp_policy(pgxp_policy_t);
int pgxp_mark_ambiguous(uint32_t);
void pgxp_resolved_sink(pgxp_resolved_sink_t, void *);
void pgxp_record_sink(pgxp_sink_t, void *);
void pgxp_visit(unsigned age, pgxp_sink_t, pgxp_mark_sink_t, void *);
#ifdef __cplusplus
}
#endif
#endif
