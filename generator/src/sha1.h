#ifndef LHG_SHA1_H
#define LHG_SHA1_H

#include <stddef.h>
#include <stdint.h>

void lhg_sha1(const uint8_t *data, size_t size, uint8_t digest[20]);
/* Writes 40 lowercase hex digits and a NUL. */
void lhg_sha1_hex(const uint8_t *data, size_t size, char hex[41]);
uint32_t lhg_fnv1a(const uint8_t *data, size_t size);
uint64_t lhg_fnv64_add(uint64_t state, const uint8_t *data, size_t size);
#define LHG_FNV64_INIT UINT64_C(14695981039346656037)

#endif
