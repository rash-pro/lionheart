#ifndef FF8_HOST_MOVIES_H
#define FF8_HOST_MOVIES_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
#include "cpu.h"
int host_movies_start(const char *directory, unsigned scale, const char *statistics);
void host_movies_bind(cpu_t *, unsigned disc);
void host_movies_restore(cpu_t *, unsigned disc);
void host_movies_sector(cpu_t *);
void host_movies_decode(void);
int host_movies_active(void);
void host_movies_stop(void);
#ifdef __cplusplus
}
#include <memory>
#include <vector>
namespace HostMovies {
struct Frame { unsigned width, height; double seconds; std::vector<uint8_t> rgba; };
struct Patch {
    unsigned x, y, words, rows, origin_x, origin_y;
    bool rgb24;
    std::shared_ptr<const Frame> frame;
};
bool span(unsigned x, unsigned y, const uint8_t *data, unsigned words, Patch &);
}
#endif
#endif
