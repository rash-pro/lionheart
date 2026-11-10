#include "../../../runtime/spu.c"
void decode(const uint8_t *data, int16_t *history, int16_t *out) { decode_block(data, history, out); }
int32_t coefficient(int32_t a, int32_t b) { return mul(a, b); }
int32_t pcm(int32_t a, int32_t b) { return mul_sample(a, b); }
