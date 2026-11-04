#ifndef FF8_SPU_H
#define FF8_SPU_H

#include <stdint.h>
#include <stdio.h>

#define SPU_RATE 44100u
#define SPU_SAMPLE_CYCLES 768u
typedef struct spu spu_t;
/* Output-only observer. Guest reads/IRQs always come from the portable model.
 * Install before advancing the device; callbacks must copy their payload. */
typedef struct {
    void (*write)(void *, unsigned offset, uint16_t value);
    void (*upload)(void *, uint32_t address, const uint8_t *, unsigned bytes);
} spu_backend_t;

spu_t *spu_create(void);
void spu_destroy(spu_t *);
void spu_set_irq(spu_t *, void (*raise)(void *), void *context);
void spu_set_backend(spu_t *, const spu_backend_t *, void *context);
/* Absolute CPU time; the hardware scheduler includes spu_next_event(). */
uint64_t spu_next_event(const spu_t *);
uint64_t spu_batch_deadline(spu_t *);
/* Conservative RAM-IRQ horizon; observing it does not advance audio/devices. */
uint64_t spu_irq_deadline(spu_t *);
unsigned spu_dma_irq_words(const spu_t *);
void spu_advance(spu_t *, uint64_t cycles);
/* Halfword registers, byte offsets from 1F801C00h. */
uint16_t spu_read(spu_t *, unsigned offset);
void spu_write(spu_t *, unsigned offset, uint16_t value);
uint32_t spu_dma_read(spu_t *);
void spu_dma_write(spu_t *, uint32_t value);
void spu_cd_input(spu_t *, const int16_t *stereo_44100, unsigned frames);
/* Full mode returns mixed PCM; with a backend installed this returns consumed
 * raw CD PCM for the hardware input, including silence at empty intervals. */
unsigned spu_read_frames(spu_t *, int16_t *stereo_44100, unsigned capacity);
void spu_control_summary(const spu_t *, FILE *);
void spu_summary(const spu_t *, FILE *);
/* Materialize deferred sample state for a RAM observation or checkpoint. */
void spu_sync(spu_t *);
const uint8_t *spu_ram(const spu_t *);

#endif
