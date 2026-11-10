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
/* Whether pc names an instruction of an overlay image loaded at base, as the
 * generator requires of a catalog entry hint. */
int metadata_entry_valid(const void *data, size_t size, uint32_t base,
                         uint32_t pc);
/* The load address the generator infers for an effect image with none
 * configured; 0 with an error when the image does not decide it. */
uint32_t metadata_effect_base(const void *data, size_t size, const char *name,
                              char *error);
/* The length of the code that starts a decompressed asset loaded at base, as
 * the generator discovers it within limit bytes; -1 with an error. */
int64_t metadata_code_prefix(const void *data, size_t size, uint32_t base,
                             size_t limit, char *error);
#ifdef __cplusplus
}
#endif
#endif
