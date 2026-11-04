/* Optional host instrumentation, never linked into the ordinary host. */
#include "spu.h"
#include <stdlib.h>
static FILE *trace;
static void record(unsigned kind, unsigned arg, unsigned value) {
    uint32_t row[3] = {kind, arg, value};
    if (trace && fwrite(row, sizeof(row), 1, trace) != 1) abort();
}
spu_t *__real_spu_create(void);
void __real_spu_destroy(spu_t *);
void __real_spu_advance(spu_t *, uint64_t);
uint16_t __real_spu_read(spu_t *, unsigned);
void __real_spu_write(spu_t *, unsigned, uint16_t);
uint32_t __real_spu_dma_read(spu_t *);
void __real_spu_dma_write(spu_t *, uint32_t);
void __real_spu_cd_input(spu_t *, const int16_t *, unsigned);
spu_t *__wrap_spu_create(void) {
    const char *path = getenv("FF8_SPU_TRACE");
    if (path) { trace = fopen(path, "wb"); if (!trace || fwrite("FF8SPUT1", 8, 1, trace) != 1) abort(); }
    return __real_spu_create();
}
void __wrap_spu_destroy(spu_t *s) { __real_spu_destroy(s); if (trace && fclose(trace)) abort(); trace = NULL; }
void __wrap_spu_advance(spu_t *s, uint64_t cycles) {
    if (trace && cycles >= spu_next_event(s)) record(1, (uint32_t)cycles, (uint32_t)(cycles >> 32));
    __real_spu_advance(s, cycles);
}
uint16_t __wrap_spu_read(spu_t *s, unsigned offset) { uint16_t value = __real_spu_read(s, offset); record(2, offset, value); return value; }
void __wrap_spu_write(spu_t *s, unsigned offset, uint16_t value) { record(3, offset, value); __real_spu_write(s, offset, value); }
uint32_t __wrap_spu_dma_read(spu_t *s) { uint32_t value = __real_spu_dma_read(s); record(4, 0, value); return value; }
void __wrap_spu_dma_write(spu_t *s, uint32_t value) { record(5, 0, value); __real_spu_dma_write(s, value); }
void __wrap_spu_cd_input(spu_t *s, const int16_t *pcm, unsigned frames) {
    record(6, frames, 0);
    if (trace && fwrite(pcm, 4, frames, trace) != frames) abort();
    __real_spu_cd_input(s, pcm, frames);
}
