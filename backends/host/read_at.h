#ifndef HOST_READ_AT_H
#define HOST_READ_AT_H
#include <stdio.h>
#include <stdint.h>
#include <unistd.h>
static inline int host_read_at(FILE *file, void *data, size_t size, uint64_t at) {
#ifdef _WIN32
    /* Only the guest thread owns this stream. Preserve the buffered FILE cursor. */
    int64_t saved=_ftelli64(file);
    int ok=saved>=0 && !_fseeki64(file,(int64_t)at,SEEK_SET) && fread(data,1,size,file)==size;
    if (saved>=0 && _fseeki64(file,saved,SEEK_SET)) ok=0;
    return ok;
#else
    return pread(fileno(file),data,size,(off_t)at)==(ssize_t)size;
#endif
}
#endif
