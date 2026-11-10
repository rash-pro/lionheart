/* Finds the compiler native code is built with, or installs the player's own
 * on a worker thread while the interface keeps drawing, and remembers which
 * one so nothing else has to look again. The interface polls a status;
 * nothing here calls it back.
 *
 * The installed Zig comes first: once a player has it, a system compiler
 * installed later changes nothing. Without it, Linux takes Clang, then GCC,
 * from the PATH, each only after it has built a module this player accepts.
 * Removing that compiler later makes the player ask for the download again,
 * as if there had never been one; the modules it built stay unused and go
 * with the next build. Windows has only the download. */
#ifndef HOST_NATIVE_SETUP_H
#define HOST_NATIVE_SETUP_H

#include <stddef.h>
#include <stdint.h>
#include "native_build.h"
#include "toolchain.h"

typedef enum {
    NATIVE_SETUP_IDLE = 0,
    NATIVE_SETUP_RUNNING,
    NATIVE_SETUP_FAILED,
    /* A compiler is there: installed, or found on the system. */
    NATIVE_SETUP_READY
} native_setup_state_t;

typedef struct {
    native_setup_state_t state;
    /* While running: the installer's phase and how far it is. */
    host_tc_phase phase;
    uint64_t done, total;
    /* The last attempt read a local archive rather than downloading. */
    int local;
    /* Ready with a compiler the system had: nothing was installed or asked. */
    int system;
    char reason[256];
} native_setup_status_t;

typedef struct {
    native_build_family_t family;
    char path[4096];
    /* Names the compiler build for the cache's identity: the installed
     * archive's checksum, or what a system compiler says it is. */
    char identity[160];
    /* For people: the archive's version, or the system compiler's. */
    char version[32];
    int system;
} native_setup_compiler_t;

/* The pinned manifest, or for tests a fixture named by
 * LIONHEART_TOOLCHAIN_TEST_MANIFEST. A fixture may only download from this
 * machine; one that cannot be read keeps the pinned identity and no address. */
const host_tc_manifest *native_setup_manifest(void);
/* Checks what <data>/toolchain holds, which reads every installed file once,
 * then, without it, the system's compilers. */
void native_setup_configure(const char *data);
/* archive NULL downloads. Returns 0 when one is already running. */
int native_setup_install(const char *archive);
void native_setup_cancel(void);
void native_setup_status(native_setup_status_t *status);
/* The compiler to build with, or 0 when there is none. */
int native_setup_compiler(native_setup_compiler_t *compiler);
/* Cancels a running installation and waits for it. */
void native_setup_shutdown(void);

#endif
