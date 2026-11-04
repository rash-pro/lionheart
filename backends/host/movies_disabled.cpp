#include "movies.h"
extern "C" int host_movies_start(const char *dir,unsigned scale,const char *stats) { (void)scale; return !dir && !stats; }
extern "C" void host_movies_bind(cpu_t *c,unsigned d) { (void)c; (void)d; }
extern "C" void host_movies_restore(cpu_t *c,unsigned d) { (void)c; (void)d; }
extern "C" void host_movies_sector(cpu_t *c) { (void)c; }
extern "C" void host_movies_decode(void) {}
extern "C" int host_movies_active(void) { return 0; }
extern "C" void host_movies_stop(void) {}
namespace HostMovies { bool span(unsigned x,unsigned y,const uint8_t *d,unsigned w,Patch &p) {
    (void)x; (void)y; (void)d; (void)w; (void)p; return false;
} }
