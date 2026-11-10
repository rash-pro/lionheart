/* Builds the player's native code in the background: one module per overlay
 * set, generated from the player's own discs, compiled with the installed
 * compiler, kept in a cache and handed to the module loader while the game
 * runs. Independent of SDL and the GUI; a screen polls native_build_status.
 *
 * The cache, under <root>/<cache id>/<executable id>/:
 *   lock              the builder holds it; other players only load
 *   include/          headers modules compile against
 *   modules/KEY.so    one module per distinct code content (.dll on Windows)
 *   modules/KEY.ok    its receipt, written last: no receipt, no module
 *   sets/IDENTITY     which module serves a set, by the set's catalog identity
 *   failed/IDENTITY   a set whose module failed, with the compiler's last words
 *   work/             builds in progress, removed by the next builder
 * <root>/<cache id>/in-use is held shared by every player using that folder,
 * so an unused folder from another player version can be removed. */
#ifndef HOST_NATIVE_BUILD_H
#define HOST_NATIVE_BUILD_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Names how modules are compiled and linked; part of the cache folder's
 * identity, so changing any compiler option must change it. */
#define NATIVE_BUILD_RECIPE "zig cc -O2 -mcpu=baseline -fPIC, -shared -nostdlib; 1"

typedef enum {
    NATIVE_BUILD_OFF = 0,
    /* Reading the cache, then the discs. */
    NATIVE_BUILD_LOADING,
    NATIVE_BUILD_BUILDING,
    /* Another player owns this cache; this one loads what it publishes. */
    NATIVE_BUILD_WAITING,
    /* Every set is native or recorded as failed. */
    NATIVE_BUILD_DONE,
    /* Something outside any one set failed (disc, disk, compiler); what was
     * loaded stays loaded and the rest stays interpreted. */
    NATIVE_BUILD_STOPPED
} native_build_state_t;

typedef struct {
    native_build_state_t state;
    /* Sets of the discs the player knows, the executable included. */
    unsigned sets, ready, failed;
    /* Group names being compiled now, comma-separated. */
    char compiling[96];
    /* Estimate for the remaining sets; -1 until one module has been built. */
    int seconds_left;
    char error[256];
    /* For measurements: modules compiled in this run, sets served by a module
     * that another set had already produced, sets found in the cache. */
    unsigned compiled, shared, cached, workers;
    /* Seconds from start until the cache had been read and every cached
     * module handed over; until the first tier was native; until done. */
    double cached_seconds, first_tier_seconds, done_seconds;
    /* Processor seconds spent by then, compilers included where the system
     * reports them to the player (Linux). */
    double first_tier_cpu, done_cpu;
} native_build_status_t;

/* One set as the generator lists it. Row 0 is the executable. */
typedef struct {
    int32_t index;
    unsigned discs;
    int effect;
    uint32_t base, size;
    /* Relative compile cost, for the estimate; 0 when unknown. */
    uint32_t work;
    char name[32];
    char identity[65];
} native_build_set_t;

/* Where sets come from. plan reads metadata only; open may take seconds and
 * must precede emit and contains, whose rows are plan's rows. */
typedef struct {
    void *user;
    int (*plan)(void *user, unsigned discs, native_build_set_t **rows, size_t *count, char *error, size_t size);
    int (*open)(void *user, char *error, size_t size);
    /* Writes row `set`'s sources into directory and names their content. */
    int (*emit)(void *user, size_t set, const char *directory, char key[65], char *error, size_t size);
    /* Whether row `set`'s image holds these bytes at pc. */
    int (*contains)(void *user, size_t set, uint32_t pc, const uint8_t *bytes, size_t size);
    void (*close)(void *user);
} native_build_source_t;

typedef struct {
    const char *name;
    const void *data;
    size_t size;
} native_build_file_t;

typedef struct {
    /* <data>/native, and the two folder names below it. The cache id names
     * everything that shapes module code: headers, generator, catalog and
     * compiler. */
    const char *root, *cache_id, *exe_id;
    /* argv[0] and the arguments before the compiler's own options ("cc"). */
    const char *compiler;
    const char *const *driver;
    size_t driver_count;
    const char *target;
    int windows;
    const char *abi_id;
    /* Written to include/; any .c among them is compiled into every module. */
    const native_build_file_t *files;
    size_t file_count;
    /* Discs the player knows (bit n for disc n) and the inserted one. */
    unsigned discs, disc;
    /* 0 picks from processors and available memory. */
    unsigned workers;
    const native_build_source_t *source;
    /* Opens and publishes a module for set `index`; 0 refuses it. */
    int (*deliver)(void *user, const char *path, int32_t index, char *error, size_t size);
    void *deliver_user;
    /* Progress lines on stderr. */
    int verbose;
} native_build_config_t;

/* Returns 0 only when the configuration cannot work at all; everything else,
 * including a cache that cannot be written, is reported through the status. */
int native_build_start(const native_build_config_t *config, char *error, size_t size);
/* Kills running compilers, waits for the workers, closes the source. */
void native_build_stop(void);
void native_build_status(native_build_status_t *status);
/* The inserted disc changed; its sets come next. */
void native_build_disc(unsigned disc);
/* The guest entered code that has no native body at pc: the first bytes
 * there are enough to tell which pending set it belongs to. Cheap, lock-free,
 * callable from the guest thread at any rate. */
void native_build_ran(uint32_t pc, const uint8_t *bytes, size_t size);
/* Blocks until done or stopped, or the timeout (seconds, <0 forever) passes. */
native_build_state_t native_build_wait(double seconds);

#ifdef __cplusplus
}
#endif

#endif
