#ifndef HOST_RA_STORE_H
#define HOST_RA_STORE_H
#include <stddef.h>
#include <stdint.h>
char *host_ra_file_read(const char *, size_t *, size_t limit, int private_file);
int host_ra_file_write(const char *, const void *, size_t);
int host_ra_credentials_read(const char *, char *user, size_t, char *token, size_t);
int host_ra_credentials_write(const char *, const char *user, const char *token);
/* Decode a bounded PNG to the common 64x64 RGBA badge used by both presenters. */
uint8_t *host_ra_badge_decode(const void *, size_t);
#endif
