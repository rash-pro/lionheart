#ifndef FF8_FRAME_ORACLE_H
#define FF8_FRAME_ORACLE_H
/* An independent judge of the frame identity service, for developer builds
 * configured with FF8_FRAME_ORACLE; the player never contains it.
 *
 * It follows, instruction by instruction in the interpreter only, which RAM
 * halfword each value was copied from: through registers, memory, the
 * scratchpad and the GTE's vertex and matrix registers, across the packing the
 * game uses (LHU, SLL 16, OR, ANDI 0xffff), and through additions with one
 * untracked operand, which keep their source marked as offset. It also follows
 * each projection's screen coordinates from the GTE's SXY registers through
 * registers and memory into the GPU packets that draw them. Every source is
 * checked against the live value before use, so writes it does not see (DMA,
 * BIOS code in C) cannot leave stale ones.
 *
 * It then judges each frame the service delivers: a pair is correct when the
 * two projections' inputs came from the same halfwords, wrong when they came
 * from different halfwords that were alive in both frames; a corner's link is
 * correct when the packet word came from the linked projection. The service
 * never reads any of this. */
#include "cpu.h"

#ifdef FF8_FRAME_ORACLE
#include "frame_identity.h"
int frame_oracle_start(cpu_t *c, const char *report);
void frame_oracle_restored(cpu_t *c);
void frame_oracle_judge(const frame_identity_frame_t *frame);
/* Writes the totals; 0 if the report could not be written or the oracle lost
 * track of the service (native code ran, or counts disagree). */
int frame_oracle_stop(void);

/* Hooks, compiled in only with FF8_FRAME_ORACLE. */
void frame_oracle_step(cpu_t *c, uint32_t word, uint32_t a, uint32_t b, uint32_t address,
                       unsigned dst, uint32_t result, unsigned load, uint32_t loaded);
/* The GPU stored a packet word in slot `slot`, read from `source` in RAM, or
 * from the address frame_oracle_dma_word named, or from neither. */
void frame_oracle_packet_word(unsigned slot, const uint8_t *source);
void frame_oracle_dma_word(uint32_t address);
void frame_oracle_polygon(unsigned opcode);
#define FRAME_ORACLE(...) __VA_ARGS__
#else
#define FRAME_ORACLE(...) ((void)0)
#endif
#endif
