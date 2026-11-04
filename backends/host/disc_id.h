#ifndef FF8_HOST_DISC_ID_H
#define FF8_HOST_DISC_ID_H
#include "disc_iso.h"
typedef struct {
    const char *serial, *exe_sha1, *image;
    uint32_t image_lba, image_size;
    const char *image_sha1;
} host_disc_metadata_t;
/* The prepared path lives until cleanup; BOM CUEs own a private normalized copy.
 * Pass it to runtime cdrom_open/swap, never replace the user's configured path. */
typedef struct { char path[4096]; int temporary; } host_disc_runtime_t;
int host_disc_runtime_path(const char *, host_disc_runtime_t *, char *error, size_t);
void host_disc_runtime_cleanup(host_disc_runtime_t *);
/* Disc-derived identifiers must be sanitized at diagnostic/UI boundaries. */
void host_disc_sanitize(char *out, size_t, const char *input);
/* Owns only file, offset, sectors. Close file with fclose after use. */
int host_disc_open(const char *, hw_cd_t *, char *error, size_t);
int host_disc_identify(const char *, int full, unsigned *number, char *error, size_t);
int host_disc_exe(const char *, uint8_t **data, size_t *size, char *error, size_t);
int host_exe_check(const uint8_t *, size_t, char *error, size_t);
/* Insert by content, rejecting duplicate disc numbers. Slots may be sparse. */
int host_disc_assign(const char **slots, const char *path, int full, char *error, size_t);
#endif
