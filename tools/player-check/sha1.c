#include "../../backends/host/sha1_stream.h"
void player_check_sha1(const void *bytes, size_t size, char out[41])
{
    static const char hex[] = "0123456789abcdef";
    host_sha1_t state;
    uint8_t hash[20];
    host_sha1_init(&state);
    host_sha1_update(&state, bytes, size);
    host_sha1_final(&state, hash);
    for (size_t i = 0; i < 20; ++i) {
        out[i * 2] = hex[hash[i] >> 4];
        out[i * 2 + 1] = hex[hash[i] & 15];
    }
    out[40] = 0;
}
