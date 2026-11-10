/* Loading recompiled code at run time, one module per overlay set. Opening
 * verifies and binds a module and may run on any thread; only committing changes
 * what the guest executes, and that happens at a dispatch boundary. */
#ifndef HOST_NATIVE_MODULES_H
#define HOST_NATIVE_MODULES_H

#include <stddef.h>
#include <stdint.h>
#include "cpu.h"

typedef struct native_module native_module_t;

/* Why a module was refused, in the order the checks run. */
typedef enum {
    NATIVE_MODULE_OK = 0,
    NATIVE_MODULE_NOT_CONFIGURED,
    NATIVE_MODULE_UNREADABLE,
    NATIVE_MODULE_TRUNCATED,
    NATIVE_MODULE_LOAD_FAILED,
    NATIVE_MODULE_NO_DESCRIPTOR,
    NATIVE_MODULE_BAD_ABI,
    NATIVE_MODULE_BAD_IMAGE,
    NATIVE_MODULE_BAD_TABLES,
    NATIVE_MODULE_CODE_MISMATCH,
    NATIVE_MODULE_MISSING_IMPORT
} native_module_status_t;

/* The EXE this run booted: its identity, and its text as it left the disc, kept
 * so the main module can be checked however late it arrives. */
int native_modules_configure(uint32_t fnv, uint32_t base, uint32_t size, const uint8_t *text);
void native_modules_shutdown(cpu_t *c);

native_module_status_t native_module_open(const char *path, native_module_t **module, char *error, size_t error_size);
void native_module_close(native_module_t *module);
/* Queues an opened module; it takes effect at the next dispatch boundary. */
void native_module_publish(native_module_t *module);
/* Registers everything queued. Only call while no guest code or lookup is running. */
void native_modules_commit(cpu_t *c);
size_t native_modules_registered(void);

/* Developer path: a directory of modules, handed over a few at a time. */
int native_modules_scan(const char *directory);
size_t native_modules_feed(size_t count);
size_t native_modules_remaining(void);

#endif
