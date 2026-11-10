#ifndef METADATA_IO_H
#define METADATA_IO_H
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef struct metadata_image metadata_image;
metadata_image *metadata_open(const char *cue, char *error);
void metadata_close(metadata_image *image);
int metadata_number(metadata_image *image);
uint32_t metadata_lba(metadata_image *image);
uint32_t metadata_size(metadata_image *image);
void *metadata_read(metadata_image *image, int64_t sector, int64_t size,
                    char *error);
void *metadata_decode(const void *input, size_t size, size_t *output_size,
                      char *error);
void *metadata_exe(metadata_image *image, const char *name, size_t *size,
                   char *error);
void metadata_hash(const void *data, size_t size, char hex[41]);
#ifdef __cplusplus
}
#endif
#endif
