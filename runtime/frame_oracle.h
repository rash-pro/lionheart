#ifndef FF8_FRAME_ORACLE_H
#define FF8_FRAME_ORACLE_H
#include <stdint.h>
struct cpu;
struct frame_oracle;
#if defined(FF8_RECORD_FRAME_ORACLE)
#define FRAME_ORACLE(...) do { __VA_ARGS__; } while (0)
int frame_oracle_enable(struct cpu *);
void frame_oracle_destroy(struct cpu *);
void frame_oracle_instruction(struct cpu *, uint32_t, uint32_t, uint32_t);
void frame_oracle_projection(struct cpu *, unsigned);
void frame_oracle_write(struct cpu *, uint32_t, unsigned);
void frame_oracle_reload(struct cpu *, uint32_t, unsigned);
void frame_oracle_irq(struct cpu *, int);
#else
#define FRAME_ORACLE(...) ((void)0)
#endif
#endif
