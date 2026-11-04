#ifndef FF8_HOST_DISC_ISO_H
#define FF8_HOST_DISC_ISO_H
#include "hw.h"
/* These reads preserve FILE position and all guest CD state. Root files only. */
int host_iso_sector(const hw_cd_t *, uint32_t lba, uint8_t out[2048]);
int host_iso_find(const hw_cd_t *, const char *name, uint32_t *lba, uint32_t *size);
int host_iso_read(const hw_cd_t *, uint32_t lba, uint32_t size, uint8_t *out);
/* BOOT may name a subdirectory; identify classifies that as another game. */
int host_disc_boot(const hw_cd_t *, char name[64]);
#endif
