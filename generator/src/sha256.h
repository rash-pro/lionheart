#ifndef LHG_SHA256_H
#define LHG_SHA256_H
#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint32_t state[8];
    uint64_t size;
    uint8_t block[64];
    size_t used;
} lhg_sha256;
void lhg_sha256_init(lhg_sha256 *hash);
void lhg_sha256_add(lhg_sha256 *hash, const void *data, size_t size);
void lhg_sha256_finish(lhg_sha256 *hash, char hex[65]);
#endif
