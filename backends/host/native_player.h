/* The release player's side of the build service: the generator as the
 * source of sets, the module loader as their destination, and the compiler
 * the player installed. Without that compiler none of this runs and the
 * player stays on the interpreter, exactly as before. */
#ifndef HOST_NATIVE_PLAYER_H
#define HOST_NATIVE_PLAYER_H

#include <stddef.h>
#include "cpu.h"
#include "native_build.h"

typedef struct {
    const char *name;
    const unsigned char *data;
    size_t size;
} host_embedded_file_t;

/* Written at build time from the catalog and runtime headers in the source tree. */
extern const host_embedded_file_t host_native_catalog[], host_native_headers[];
extern const size_t host_native_catalog_count, host_native_headers_count;
extern const char host_native_catalog_digest[];

typedef struct {
    int verbose;
    unsigned workers;
    /* Developers only: another compiler driver in place of the installed one. */
    const char *compiler;
} host_native_options_t;

/* Keeps the booted executable's identity and text, which are intact only
 * before the first instruction runs. Building can start any time after. */
int host_native_boot(cpu_t *c);
/* Starts building for the discs the player knows (by number, NULL when
 * absent) with the compiler native_setup found or installed. Returns 0 when
 * there is nothing to start: no compiler, no data directory, not booted. */
int host_native_start(const char *data, const char *const discs[4], unsigned inserted, const host_native_options_t *options);
/* The inserted disc changed. */
void host_native_disc(unsigned disc);
/* The discs the player knows changed: building starts again on the new list;
 * modules already loaded stay loaded. */
void host_native_discs(const char *const discs[4], unsigned inserted);
/* wait: let the build finish first, for headless runs that want a whole cache. */
void host_native_stop(cpu_t *c, int wait);
int host_native_running(void);

#endif
