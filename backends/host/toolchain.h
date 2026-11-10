#ifndef HOST_TOOLCHAIN_H
#define HOST_TOOLCHAIN_H
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

typedef enum { HOST_TC_ABSENT, HOST_TC_PARTIAL, HOST_TC_INSTALLED } host_tc_state;
typedef enum { HOST_TC_DOWNLOAD, HOST_TC_VERIFY, HOST_TC_EXTRACT,
               HOST_TC_CHECK, HOST_TC_COMMIT } host_tc_phase;
/* Return nonzero to cancel. Called synchronously on the installing thread;
 * marshal these values to the UI rather than calling UI functions here. */
typedef int (*host_tc_progress)(void *, host_tc_phase, uint64_t, uint64_t);
typedef struct {
    const char *version, *format, *prefix, *executable, *sha256;
    uint64_t size;
    const char *const *urls;
    size_t url_count;
    const char *const *files;
    size_t file_count;
} host_tc_manifest;
typedef struct { char reason[256]; } host_tc_error;
/* The shipped manifest is immutable; overrides are for developer fixtures. */
const host_tc_manifest *host_tc_pinned(void);
host_tc_state host_tc_query(const char *data, const host_tc_manifest *);
int host_tc_executable(const char *data, const host_tc_manifest *, char *, size_t);
/* local_archive == NULL selects download. A busy installer fails with a retryable
 * reason. Cancelled downloads retain their prefix; extraction restarts cleanly.
 * Separate calls/threads own their callbacks, errors and file handles. */
int host_tc_install(const char *data, const host_tc_manifest *, const char *local_archive,
                    host_tc_progress, void *, host_tc_error *);
#ifdef __cplusplus
}
#endif
#endif
