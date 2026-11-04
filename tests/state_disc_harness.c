#define _POSIX_C_SOURCE 200809L
#include "state_disc.h"
#include <assert.h>
#include <string.h>
#include <unistd.h>
static unsigned reads;
ssize_t __real_pread(int, void *, size_t, off_t);
ssize_t __wrap_pread(int fd, void *p, size_t n, off_t at) { ++reads; return __real_pread(fd,p,n,at); }
static void print_hash(const uint8_t *hash) { unsigned i; for (i = 0; i < 20; ++i) printf("%02x",hash[i]); putchar('\n'); }
int main(int argc, char **argv) {
    hw_cd_t d = {0};
    uint8_t quick[20], full[20], again[20], byte = 42;
    unsigned before;
    assert(argc == 2);
    d.file = fopen(argv[1],"r+b"); assert(d.file);
    assert(!fseek(d.file,0,SEEK_END)); d.sectors = (uint32_t)(ftell(d.file) / 2352);
    assert(!fseek(d.file,123,SEEK_SET));
    assert(host_state_disc_hash(&d,0,quick) && reads > 0 && ftell(d.file) == 123);
    print_hash(quick);
    before = reads;
    assert(host_state_disc_hash(&d,0,again) && !memcmp(quick,again,20) && reads == before);
    /* Reopening the same immutable image during load reuses the session cache. */
    fclose(d.file); d.file = fopen(argv[1],"r+b"); assert(d.file);
    assert(host_state_disc_hash(&d,0,again) && !memcmp(quick,again,20) && reads == before);
    assert(host_state_disc_hash(&d,1,full) && reads > before); print_hash(full);
    before = reads;
    assert(host_state_disc_hash(&d,1,again) && !memcmp(full,again,20) && reads == before);
    /* Metadata changes invalidate both modes; no stale result after a rewrite. */
    assert(pwrite(fileno(d.file),&byte,1,0) == 1);
    assert(host_state_disc_hash(&d,0,again) && memcmp(quick,again,20) && reads > before);
    before = reads;
    assert(host_state_disc_hash(&d,1,again) && memcmp(full,again,20) && reads > before);
    host_state_disc_clear(); fclose(d.file);
    puts("cached quick/full identities, reopen hit, metadata invalidation, unchanged FILE position: OK");
    return 0;
}
