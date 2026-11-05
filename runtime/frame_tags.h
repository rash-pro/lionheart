#ifndef FF8_FRAME_TAGS_H
#define FF8_FRAME_TAGS_H
#include <stdint.h>

/* Host shadow state. Never serialized into guest/save-state memory. */
typedef uint64_t frame_tag_t;
#define FRAME_TAG_MIXED UINT64_MAX
#define FRAME_TAG_UNKNOWN (UINT64_MAX - 1)
struct cpu;
struct gpu;
struct frame_tags;
typedef struct {
    frame_tag_t r[32], hi, lo, load;
    frame_tag_t location[32], load_location, hi_location, lo_location;
} frame_register_tags_t;

typedef struct {
    frame_tag_t vertex, context;
    uint64_t generation, collection;
    uint32_t pc, occurrence, input_xy, input_z;
    uint32_t matrix[8];
} frame_corner_t;

int frame_tags_enable(struct cpu *);
void frame_tags_destroy(struct cpu *);
void frame_tags_reset(struct cpu *);
void frame_tags_pause(struct cpu *, int);
void frame_tags_present(struct cpu *);
unsigned frame_tags_gc_hold(struct cpu *);
void frame_tags_gc_release(struct cpu *, unsigned);
frame_register_tags_t *frame_tags_registers(struct cpu *);
frame_tag_t frame_tags_merge(struct cpu *, frame_tag_t, frame_tag_t);
frame_tag_t frame_tags_load(struct cpu *, uint32_t, unsigned);
void frame_tags_invalidate(struct cpu *, uint32_t, unsigned);
void frame_tags_store(struct cpu *, uint32_t, unsigned, frame_tag_t);
void frame_tags_retire(struct cpu *, unsigned, frame_tag_t, unsigned, frame_tag_t);
frame_tag_t frame_tags_location(struct cpu *, uint32_t);
void frame_tags_location_store(struct cpu *, uint32_t, unsigned, frame_tag_t);
void frame_tags_location_retire(struct cpu *, unsigned, frame_tag_t, unsigned, frame_tag_t);
void frame_tags_reload(struct cpu *, uint32_t, unsigned);
frame_tag_t frame_tags_control_source(struct cpu *, unsigned);
void frame_tags_control_location(struct cpu *, unsigned, frame_tag_t);
void frame_tags_exception(struct cpu *);
frame_tag_t frame_tags_gte_read(struct cpu *, unsigned, int);
void frame_tags_gte_write(struct cpu *, unsigned, int, frame_tag_t);
void frame_tags_gte_command(struct cpu *, uint32_t);
void frame_tags_project(struct cpu *, unsigned, uint32_t, int64_t, int64_t, uint32_t);
void frame_tags_port(struct cpu *, frame_tag_t);
frame_tag_t frame_tags_port_read(struct cpu *);
void frame_tags_polygon(struct cpu *, unsigned, const uint32_t *, const frame_tag_t *, unsigned, int, int);
int frame_tags_triangle(struct cpu *, unsigned, frame_corner_t [3]);
void frame_tags_swap(struct cpu *, uint32_t);
void frame_tags_vblank(struct cpu *);
void frame_tags_thread_save(struct cpu *, uint32_t);
void frame_tags_thread_restore(struct cpu *, uint32_t);
void frame_tags_pipeline_save(struct cpu *, unsigned);
void frame_tags_pipeline_restore(struct cpu *, unsigned);

#if defined(FF8_FRAME_PROVENANCE)
#define FRAME_TAGS(...) do { __VA_ARGS__; } while (0)
#else
#define FRAME_TAGS(...) ((void)0)
#endif
#endif
