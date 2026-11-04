#ifndef FF8_HOST_GPU_STREAM_H
#define FF8_HOST_GPU_STREAM_H
#include <stdint.h>
static inline uint32_t gpu_stream_le32(const uint8_t *p) {
    return (uint32_t)p[0] | (uint32_t)p[1]<<8 | (uint32_t)p[2]<<16 | (uint32_t)p[3]<<24;
}
static inline void gpu_stream_put32(uint8_t *p,uint32_t v) {
    p[0]=(uint8_t)v; p[1]=(uint8_t)(v>>8); p[2]=(uint8_t)(v>>16); p[3]=(uint8_t)(v>>24);
}
#endif
