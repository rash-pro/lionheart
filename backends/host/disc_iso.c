#define _POSIX_C_SOURCE 200809L
#include "disc_iso.h"
#include <ctype.h>
#include <string.h>
#include <strings.h>
#include "read_at.h"

static uint32_t le32(const uint8_t *p) {
    return p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
int host_iso_sector(const hw_cd_t *d, uint32_t lba, uint8_t out[2048]) {
    uint8_t raw[2352];
    off_t at = ((uint64_t)d->offset + lba) * 2352;
    if (lba >= d->sectors || !host_read_at(d->file,raw,sizeof(raw),(uint64_t)at) ||
        (raw[15] != 1 && raw[15] != 2)) return 0;
    memcpy(out,raw + (raw[15] == 1 ? 16 : 24),2048); return 1;
}
int host_iso_find(const hw_cd_t *d, const char *name, uint32_t *lba, uint32_t *size) {
    uint8_t data[2048];
    uint32_t root, bytes, i, p;
    size_t length = strlen(name);
    if (!host_iso_sector(d,16,data) || memcmp(data + 1,"CD001",5)) return 0;
    root = le32(data + 158); bytes = le32(data + 166);
    if (bytes > 1024 * 1024 || root >= d->sectors || (bytes + 2047) / 2048 > d->sectors - root) return 0;
    for (i = 0; i < (bytes + 2047) / 2048; ++i) {
        if (!host_iso_sector(d,root + i,data)) return 0;
        for (p = 0; p + 34 <= 2048 && data[p]; p += data[p]) {
            unsigned record = data[p], n = data[p + 32];
            if (record < 34 || p + record > 2048 || n > record - 33) return 0;
            if (!(data[p + 25] & 2) && n >= length && !strncasecmp((const char *)data + p + 33,name,length) &&
                (n == length || data[p + 33 + length] == ';')) {
                *lba = le32(data + p + 2); *size = le32(data + p + 10);
                return *lba < d->sectors && ((uint64_t)*size + 2047) / 2048 <= d->sectors - *lba;
            }
        }
    }
    return 0;
}
int host_iso_read(const hw_cd_t *d, uint32_t lba, uint32_t size, uint8_t *out) {
    uint8_t sector[2048];
    uint32_t pos;
    if (lba >= d->sectors || ((uint64_t)size + 2047) / 2048 > d->sectors - lba) return 0;
    for (pos = 0; pos < size;) {
        uint32_t n = size - pos < 2048 ? size - pos : 2048;
        if (!host_iso_sector(d,lba++,sector)) return 0;
        memcpy(out + pos,sector,n); pos += n;
    }
    return 1;
}
int host_disc_boot(const hw_cd_t *d, char name[64]) {
    uint8_t data[4097];
    uint32_t lba, size;
    char *line, *next;
    if (!host_iso_find(d,"SYSTEM.CNF",&lba,&size) || !size || size > sizeof(data)-1 ||
        !host_iso_read(d,lba,size,data)) return 0;
    data[size] = 0;
    for (line = (char *)data; line && *line; line = next) {
        char *boot, *end;
        size_t n;
        next = strchr(line,'\n'); if (next) *next++ = 0;
        while (isspace((unsigned char)*line)) ++line;
        if (strncasecmp(line,"BOOT",4)) continue;
        boot = line + 4;
        while (isspace((unsigned char)*boot)) ++boot;
        if (*boot++ != '=') continue;
        while (isspace((unsigned char)*boot)) ++boot;
        if (strncasecmp(boot,"cdrom:",6)) continue;
        boot += 6; while (*boot == '\\' || *boot == '/') ++boot;
        end = boot;
        while (*end && *end != ';' && !isspace((unsigned char)*end)) ++end;
        n = (size_t)(end - boot);
        if (!n || n >= 64) return 0;
        memcpy(name,boot,n); name[n] = 0; return 1;
    }
    return 0;
}
