#define _POSIX_C_SOURCE 200809L
#include "state_disc.h"
#include "disc_iso.h"
#include <stdlib.h>
#include <string.h>
#include "read_at.h"
#include "sha1_stream.h"
#include <sys/stat.h>
#include <unistd.h>

static struct {
    struct stat st;
    uint32_t offset;
    int used, full;
    uint8_t hash[20];
} cache[4];
static unsigned replacement;
static int executable_hash(const hw_cd_t *d, uint8_t hash[20]) {
    uint8_t data[2048], *exe;
    uint32_t lba, size, length, pos;
    char name[64];
    size_t n;
    int ok = 0;
    if (!host_disc_boot(d,name)) return 0;
    n = strlen(name);
    if (!host_iso_find(d,name,&lba,&size) || !host_iso_sector(d,lba,data) || memcmp(data,"PS-X EXE",8)) return 0;
    /* rc_hash-style selection: filename + 2048-byte header + declared payload.
     * SHA-1 here is an identity component, not the RetroAchievements MD5. */
    length = data[28] | (uint32_t)data[29] << 8 | (uint32_t)data[30] << 16 | (uint32_t)data[31] << 24;
    if (length > 16 * 1024 * 1024 - 2048 || length + 2048 > size) return 0;
    length += 2048;
    exe = malloc(n + length);
    if (!exe) return 0;
    memcpy(exe,name,n);
    for (pos = 0; pos < length; pos += 2048) {
        unsigned count = length - pos < 2048 ? length - pos : 2048;
        if (!host_iso_sector(d,lba + pos / 2048,data)) goto done;
        memcpy(exe + n + pos,data,count);
    }
    code_sha1(exe,n + length,hash); ok = 1;
done:
    free(exe); return ok;
}
int host_state_disc_hash(const hw_cd_t *d, int full, uint8_t hash[20]) {
    struct stat st;
    uint8_t data[65536];
    host_sha1_t stream;
    uint64_t at;
    uint8_t identity[96] = {'F','F','8','D','I','S','C',1};
    unsigned i;
    size_t sample;
    if (!d->file || fstat(fileno(d->file),&st) || st.st_size <= 0 || (uint64_t)st.st_size > SIZE_MAX) return 0;
#ifndef _WIN32
    for (i = 0; i < 4; ++i) if (cache[i].used && cache[i].full == full && cache[i].offset == d->offset &&
        cache[i].st.st_dev == st.st_dev && cache[i].st.st_ino == st.st_ino && cache[i].st.st_size == st.st_size &&
        cache[i].st.st_mtime == st.st_mtime && cache[i].st.st_ctime == st.st_ctime
        && cache[i].st.st_mtim.tv_nsec == st.st_mtim.tv_nsec && cache[i].st.st_ctim.tv_nsec == st.st_ctim.tv_nsec
) {
        memcpy(hash,cache[i].hash,20); return 1;
    }
#endif
    if (full) {
        host_sha1_init(&stream);
        for (at=0;at<(uint64_t)st.st_size;) {
            size_t n=(uint64_t)st.st_size-at<sizeof(data) ? (size_t)((uint64_t)st.st_size-at) : sizeof(data);
            if (!host_read_at(d->file,data,n,at)) return 0;
            host_sha1_update(&stream,data,n); at+=n;
        }
        host_sha1_final(&stream,hash);
    }
    else {
        for (i = 0; i < 8; ++i) identity[8+i] = (uint8_t)((uint64_t)st.st_size >> (i * 8));
        for (i = 0; i < 4; ++i) identity[16+i] = (uint8_t)(d->offset >> (i * 8));
        sample = st.st_size < 1024 * 1024 ? (size_t)st.st_size : 1024 * 1024;
        for (unsigned part=0;part<2;++part) {
            host_sha1_init(&stream);
            for (at=0;at<sample;) {
                size_t n=sample-at<sizeof(data) ? (size_t)(sample-at) : sizeof(data);
                if (!host_read_at(d->file,data,n,(part ? (uint64_t)st.st_size-sample : 0)+at)) return 0;
                host_sha1_update(&stream,data,n); at+=n;
            }
            host_sha1_final(&stream,identity+20+part*20);
        }
        if (!executable_hash(d,identity + 60)) return 0;
        code_sha1(identity,sizeof(identity),hash);
    }
    i = replacement++ % 4; cache[i].st = st; cache[i].offset = d->offset; cache[i].full = full; cache[i].used = 1;
    memcpy(cache[i].hash,hash,20); return 1;
}
void host_state_disc_clear(void) { memset(cache,0,sizeof(cache)); replacement = 0; }
