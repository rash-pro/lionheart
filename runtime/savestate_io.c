#include <stddef.h>
#if defined(FF8_SAVESTATES)
#include "savestate_internal.h"
void ss_version(ss_io *io, uint32_t version) {
    if (io->reading && io->version != version) io->failed = 1;
    if (!io->reading) io->version = version;
}
void ss_values(ss_io *io, void *p, size_t width, size_t count) {
    size_t i, j, bytes;
    if (io->failed) return;
    if (count > SAVESTATE_MAX / width) { io->failed = 1; return; }
    bytes = width * count;
    if (io->pos > SAVESTATE_MAX - bytes || (io->reading && bytes > io->size - io->pos)) { io->failed = 1; return; }
    if (!io->reading && io->pos + bytes > io->capacity) {
        size_t cap = io->pos + bytes + 4096;
        uint8_t *data = realloc(io->data, cap);
        if (!data) { io->failed = 1; return; }
        io->data = data; io->capacity = cap;
    }
    /* Byte arrays dominate states. On little-endian hosts all scalar arrays
     * already have their canonical encoding; the fallback keeps big endian portable. */
    { const uint16_t little = 1;
      if (width == 1 || *(const uint8_t *)&little) {
        if (io->reading) memcpy(p,io->data + io->pos,bytes);
        else memcpy(io->data + io->pos,p,bytes);
        io->pos += bytes;
        if (!io->reading) io->size = io->pos;
        return;
      }
    }
    for (i = 0; i < count; ++i) {
        uint64_t value = 0;
        uint8_t *at = (uint8_t *)p + i * width;
        if (!io->reading) {
            if (width == 1) value = *at;
            else if (width == 2) { uint16_t v; memcpy(&v, at, 2); value = v; }
            else if (width == 4) { uint32_t v; memcpy(&v, at, 4); value = v; }
            else memcpy(&value, at, 8);
        }
        for (j = 0; j < width; ++j) {
            if (io->reading) value |= (uint64_t)io->data[io->pos++] << (j * 8);
            else io->data[io->pos++] = (uint8_t)(value >> (j * 8));
        }
        if (io->reading) {
            if (width == 1) *at = (uint8_t)value;
            else if (width == 2) { uint16_t v = (uint16_t)value; memcpy(at, &v, 2); }
            else if (width == 4) { uint32_t v = (uint32_t)value; memcpy(at, &v, 4); }
            else memcpy(at, &value, 8);
        }
    }
    if (!io->reading) io->size = io->pos;
}
#endif
