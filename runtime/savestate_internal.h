#ifndef FF8_SAVESTATE_INTERNAL_H
#define FF8_SAVESTATE_INTERNAL_H
#include "savestate.h"
#include "hw.h"
#include "sio.h"
#include <string.h>
#include <stdlib.h>
typedef struct { uint8_t *data; size_t pos, size, capacity; int reading, failed; uint32_t section, version; } ss_io;
void ss_values(ss_io *, void *, size_t width, size_t count);
void ss_version(ss_io *, uint32_t);
#define S8(x) ss_values(io, &(x), 1, sizeof(x))
#define S16(x) ss_values(io, &(x), 2, sizeof(x) / 2)
#define S32(x) ss_values(io, &(x), 4, sizeof(x) / 4)
#define S64(x) ss_values(io, &(x), 8, sizeof(x) / 8)
void ss_gpu(ss_io *, gpu_t *);
void ss_spu(ss_io *, spu_t *);
void ss_mdec(ss_io *, mdec_t *);
void ss_dma(ss_io *, struct hw *);
void ss_cdrom(ss_io *, hw_cd_t *);
void ss_sio(ss_io *, cpu_t *);
void ss_sio_input(cpu_t *, int analog[2], int *scripted);
memcard_t *ss_memcard(ss_io *, memcard_t *);
int ss_card_publish(memcard_t *, FILE *);
void ss_card_hash(const memcard_t *, uint8_t hash[20]);
void ss_card_live_reset(memcard_t *, int changed);
void ss_sio_live_cards(cpu_t *next, cpu_t *current, unsigned changed);
#endif
