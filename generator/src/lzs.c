#include "disc.h"

#include <string.h>

/* FF7/FF8 LZS: u32 compressed size, then LZSS with a zero-filled 4096-byte
 * ring starting at 0xFEE, flag bytes read LSB first. A truncated final token
 * ends the stream instead of failing, because the loader reads bounded
 * prefixes of files and only the bytes it actually decodes count. */
uint8_t *lhg_lzs_decompress(lhg_ctx *ctx, const uint8_t *input, size_t input_size, size_t *size)
{
    if (input_size < 4)
        lhg_fail(ctx, "LZS stream shorter than its header");
    uint64_t declared = (uint64_t)input[0] | (uint64_t)input[1] << 8 | (uint64_t)input[2] << 16 | (uint64_t)input[3] << 24;
    size_t end = input_size;
    if (declared + 4 < input_size)
        end = (size_t)(declared + 4);
    uint8_t ring[4096];
    memset(ring, 0, sizeof ring);
    unsigned position = 0xFEE;
    size_t cursor = 4, capacity = input_size * 2 + 64, used = 0;
    uint8_t *out = lhg_alloc(ctx, capacity);
    while (cursor < end) {
        unsigned flags = input[cursor++];
        for (int bit = 0; bit < 8; bit++) {
            if (cursor >= end)
                break;
            if (flags >> bit & 1) {
                uint8_t c = input[cursor++];
                if (used >= capacity)
                    out = lhg_realloc(ctx, out, capacity *= 2);
                out[used++] = c;
                ring[position] = c;
                position = (position + 1) & 4095;
            } else {
                if (cursor + 1 >= end) {
                    cursor = end;
                    break;
                }
                unsigned low = input[cursor], high = input[cursor + 1];
                cursor += 2;
                unsigned offset = low | ((high & 0xF0) << 4);
                unsigned length = (high & 15) + 3;
                for (unsigned k = 0; k < length; k++) {
                    uint8_t c = ring[(offset + k) & 4095];
                    if (used >= capacity)
                        out = lhg_realloc(ctx, out, capacity *= 2);
                    out[used++] = c;
                    ring[position] = c;
                    position = (position + 1) & 4095;
                }
            }
        }
    }
    *size = used;
    return out;
}
