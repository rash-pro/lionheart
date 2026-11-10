/* Installs the player's compiler on a worker thread while the interface keeps
 * drawing, and remembers whether it is installed so nothing else has to hash
 * it again. The interface polls a status; nothing here calls it back. */
#ifndef HOST_NATIVE_SETUP_H
#define HOST_NATIVE_SETUP_H

#include <stddef.h>
#include <stdint.h>
#include "toolchain.h"

typedef enum {
    NATIVE_SETUP_IDLE = 0,
    NATIVE_SETUP_RUNNING,
    NATIVE_SETUP_FAILED,
    NATIVE_SETUP_INSTALLED
} native_setup_state_t;

typedef struct {
    native_setup_state_t state;
    /* While running: the installer's phase and how far it is. */
    host_tc_phase phase;
    uint64_t done, total;
    /* The last attempt read a local archive rather than downloading. */
    int local;
    char reason[256];
} native_setup_status_t;

/* The pinned manifest, or for tests a fixture named by
 * LIONHEART_TOOLCHAIN_TEST_MANIFEST. A fixture may only download from this
 * machine; one that cannot be read keeps the pinned identity and no address. */
const host_tc_manifest *native_setup_manifest(void);
/* Checks what <data>/toolchain holds; that reads every installed file once. */
void native_setup_configure(const char *data);
/* archive NULL downloads. Returns 0 when one is already running. */
int native_setup_install(const char *archive);
void native_setup_cancel(void);
void native_setup_status(native_setup_status_t *status);
/* The installed compiler, or 0 when there is none. */
int native_setup_compiler(char *path, size_t size);
/* Cancels a running installation and waits for it. */
void native_setup_shutdown(void);

#endif
